#include "peewee/venues/bybit.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include "bybit_auth.hpp"
#include "bybit_codec.hpp"
#include "peewee/core/log.hpp"
#include "peewee/core/rate_limiter.hpp"
#include "transport/http_client.hpp"
#include "transport/ws_client.hpp"

namespace peewee {

namespace asio = boost::asio;

// Endpoint (mainnet / testnet):
//   public  : wss://stream.bybit.com/v5/public/{category}  | wss://stream-testnet.bybit.com/...
//   private : wss://stream.bybit.com/v5/private            | wss://stream-testnet.bybit.com/...
//   trade   : wss://stream.bybit.com/v5/trade              | wss://stream-testnet.bybit.com/...
//   REST    : https://api.bybit.com                        | https://api-testnet.bybit.com

namespace {

constexpr auto kConnectTimeout = std::chrono::seconds(15);
// Pong every 10 s on every connection: 25 s of silence = dead connection
// (was 20 s pings / 60 s timeout: a network cut was noticed too late).
constexpr auto kPingInterval = std::chrono::seconds(10);
constexpr auto kIdleTimeout = std::chrono::seconds(25);
constexpr int kMaxBackoffSeconds = 30;
constexpr int kRecvWindowMs = 5000;
// Bybit's default order-entry limit is 10 requests/s per kind: check your account's limits.
constexpr double kRequestsPerSecond = 10.0;

int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Timestamp wall_now() {
    return std::chrono::duration_cast<Timestamp>(std::chrono::system_clock::now().time_since_epoch());
}

Timestamp steady_now() {
    return std::chrono::duration_cast<Timestamp>(std::chrono::steady_clock::now().time_since_epoch());
}

}  // namespace

// THREADING: every WebSocket callback, timer and on_event() call runs on
// `thread` (the single network thread). Engine-thread calls (subscribe_*,
// send_*, resync) are posted to it. REST calls run on `rest_pool`, because
// they block, and post their result back to the network thread.
struct BybitVenue::Impl {
    // One WebSocket connection with its reconnect state.
    struct Conn {
        Conn(asio::io_context& io, const char* name_, StreamKind kind_) : name(name_), kind(kind_), retry(io) {}

        const char* name;
        StreamKind kind;
        std::unique_ptr<transport::WsClient> ws;
        asio::steady_timer retry;
        int backoff_s = 1;
        bool ready = false;  // Open and, for private/trade, authenticated.
    };

    explicit Impl(BybitVenue& v)
        : self(v),
          ping_timer(io),
          pub(io, "public", StreamKind::Public),
          priv(io, "private", StreamKind::Private),
          trade(io, "trade", StreamKind::Trading),
          buckets{TokenBucket(kRequestsPerSecond, kRequestsPerSecond),
                  TokenBucket(kRequestsPerSecond, kRequestsPerSecond),
                  TokenBucket(kRequestsPerSecond, kRequestsPerSecond)} {}

    // --- Helpers (network thread) ---

    bool has_keys() const { return !self.cfg_.api_key.empty(); }

    void emit(Event e) { self.sink_->on_event(std::move(e)); }

    void emit_status(StreamKind kind, bool up) { emit(VenueStatus{self.id_, kind, up, wall_now()}); }

    void emit_reject(ClientOrderId cid, RequestId req, RequestKind kind, const char* text) {
        OrderReject r;
        r.cid = cid;
        r.req = req;
        r.request = kind;
        r.reason = RejectReason::VenueUnavailable;
        r.text = text;
        r.ts = wall_now();
        emit(OrderEvent{r});
    }

    // Wakes up connect(), which waits for the connections to be ready.
    void notify(bool& flag, std::string error = {}) {
        {
            std::lock_guard lock(m);
            flag = true;
            if (!error.empty()) connect_error = std::move(error);
        }
        cv.notify_all();
    }

    void subscribe(transport::WsClient& ws, const std::vector<std::string>& topics) {
        // At most 10 topics per request.
        for (size_t i = 0; i < topics.size(); i += 10) {
            const auto end = std::min(topics.size(), i + 10);
            ws.send(bybit::make_subscribe(std::span(topics.data() + i, end - i)));
        }
    }

    void authenticate(Conn& c) {
        c.ws->send(bybit::make_auth(self.cfg_.api_key, self.cfg_.api_secret, wall_ms() + 10000));
    }

    void on_closed(Conn& c, std::string_view reason) {
        PEEWEE_LOG("bybit %s: connection lost (%.*s), reconnecting in %d s", c.name,
                   static_cast<int>(reason.size()), reason.data(), c.backoff_s);
        c.ready = false;
        if (&c == &priv) private_subscribed = false;
        emit_status(c.kind, false);

        c.retry.expires_after(std::chrono::seconds(c.backoff_s));
        c.retry.async_wait([this, &c](const boost::system::error_code& ec) {
            if (!ec) c.ws->connect();
        });
        c.backoff_s = std::min(c.backoff_s * 2, kMaxBackoffSeconds);
    }

    void schedule_ping() {
        ping_timer.expires_after(kPingInterval);
        ping_timer.async_wait([this](const boost::system::error_code& ec) {
            if (ec) return;
            for (Conn* c : {&pub, &priv, &trade}) {
                if (c->ws && c->ws->is_open()) c->ws->send(bybit::make_ping());
            }
            schedule_ping();
        });
    }

    // --- Public stream: order books ---

    void on_public_open() {
        pub.backoff_s = 1;
        pub.ready = true;
        if (!book_topics.empty()) subscribe(*pub.ws, book_topics);  // Bybit answers with snapshots.
        emit_status(StreamKind::Public, true);
        notify(public_up);
    }

    void on_public_message(std::string_view msg) {
        // Fast path: book messages usually start with their topic.
        if (msg.starts_with(R"({"topic":"orderbook.)")) return on_book_message(msg);

        const auto env = bybit::parse_envelope(msg);
        if (env.topic.starts_with("orderbook.")) {
            on_book_message(msg);
        } else if (env.op == "subscribe" && !env.ok) {
            PEEWEE_LOG("bybit public: subscribe failed: %s", env.text.c_str());
        } else if (env.topic.empty() && env.op.empty()) {
            PEEWEE_LOG("bybit public: unexpected message: %.*s", static_cast<int>(std::min<size_t>(msg.size(), 200)),
                       msg.data());
        }
    }

    void on_book_message(std::string_view msg) {
        if (auto delta = bybit::parse_orderbook(msg, symbols)) {
            emit(std::move(*delta));
        } else {
            PEEWEE_LOG("bybit public: unreadable book message: %.*s",
                       static_cast<int>(std::min<size_t>(msg.size(), 200)), msg.data());
        }
    }

    // --- Private stream: orders and executions ---

    void on_private_open() {
        priv.backoff_s = 1;
        authenticate(priv);
    }

    // PEEWEE_DEBUG=1 prints every message received on the private and trade
    // streams (they never contain the API secret). Useful to save real
    // messages in tests/data/.
    void debug_print(const char* stream, std::string_view msg) {
        if (debug) PEEWEE_LOG("bybit %s <- %.*s", stream, static_cast<int>(msg.size()), msg.data());
    }

    // Order requests carry no secret (only the login does), so they can be
    // printed too.
    void send_trade(std::string msg) {
        if (debug) PEEWEE_LOG("bybit trade -> %s", msg.c_str());
        trade.ws->send(std::move(msg));
    }

    void on_private_message(std::string_view msg) {
        debug_print("private", msg);
        const auto env = bybit::parse_envelope(msg);
        if (env.topic == "order") {
            for (auto& e : bybit::parse_order_topic(msg, symbols)) emit(std::move(e));
        } else if (env.topic == "execution") {
            for (auto& e : bybit::parse_execution_topic(msg, symbols)) emit(std::move(e));
        } else if (env.op == "auth") {
            if (!env.ok) return auth_failed(priv, env.text);
            PEEWEE_LOG("bybit private: authenticated");
            priv.ready = true;
            if (private_wanted) subscribe_private_topics();
            notify(private_up);
        } else if (env.op == "subscribe") {
            if (!env.ok) {
                PEEWEE_LOG("bybit private: subscribe failed: %s", env.text.c_str());
                return;
            }
            PEEWEE_LOG("bybit private: subscribed to orders and executions");
            private_subscribed = true;
            // Private-up makes the OMS reconcile its orders (fills missed while down).
            emit_status(StreamKind::Private, true);
        }
    }

    void subscribe_private_topics() { subscribe(*priv.ws, {"order", "execution"}); }

    // --- Trade stream: order entry ---

    void on_trade_open() {
        trade.backoff_s = 1;
        authenticate(trade);
    }

    void on_trade_message(std::string_view msg) {
        debug_print("trade", msg);
        const auto env = bybit::parse_envelope(msg);
        if (env.op == "auth") {
            if (!env.ok) return auth_failed(trade, env.text);
            PEEWEE_LOG("bybit trade: authenticated");
            trade.ready = true;
            emit_status(StreamKind::Trading, true);
            notify(trade_up);
        } else if (env.op.starts_with("order.")) {
            if (env.ok) {
                PEEWEE_LOG("bybit trade: %s accepted", env.op.c_str());
            } else {
                PEEWEE_LOG("bybit trade: %s rejected: %s", env.op.c_str(), env.text.c_str());
            }
            if (auto e = bybit::parse_trade_response(msg)) emit(std::move(*e));
        }
    }

    // A wrong key does not get better by retrying: stop this connection.
    void auth_failed(Conn& c, const std::string& text) {
        PEEWEE_LOG("bybit %s: authentication failed: %s", c.name, text.c_str());
        c.ws->close();
        bool ignored = false;
        notify(ignored, std::string("authentication failed on ") + c.name + ": " + text);
    }

    // --- Members (io first: everything below uses it, so it is destroyed last) ---

    BybitVenue& self;
    asio::io_context io;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::thread thread;
    asio::steady_timer ping_timer;
    Conn pub;
    Conn priv;
    Conn trade;

    asio::thread_pool rest_pool{1};
    std::unique_ptr<transport::HttpClient> rest;  // Used only on rest_pool.

    bybit::SymbolMap symbols;              // Built in connect(), read-only afterwards.
    std::vector<std::string> book_topics;  // Network thread.
    bool private_wanted = false;           // Network thread.
    bool private_subscribed = false;       // Network thread.
    const bool debug = std::getenv("PEEWEE_DEBUG") != nullptr;

    TokenBucket buckets[3];  // Engine thread, indexed by RequestKind.

    // connect() waits for these.
    std::mutex m;
    std::condition_variable cv;
    bool public_up = false;
    bool private_up = false;
    bool trade_up = false;
    std::string connect_error;
};

BybitVenue::BybitVenue(VenueId id, BybitConfig cfg)
    : id_(id), cfg_(std::move(cfg)), impl_(std::make_unique<Impl>(*this)) {
    caps_.native_amend = true;
    caps_.amend_keeps_priority = false;
    caps_.ws_order_entry = true;
    caps_.post_only = true;
    caps_.book_checksum = false;
    caps_.max_book_depth = 1000;
}

BybitVenue::~BybitVenue() {
    disconnect();
}

void BybitVenue::add_instrument(const Instrument& inst) {
    instruments_[inst.id] = inst;
    by_symbol_[inst.symbol] = inst.id;
}

BybitInstrumentSpec BybitVenue::fetch_instrument(std::string_view symbol) const {
    transport::HttpClient http(cfg_.testnet ? "https://api-testnet.bybit.com" : "https://api.bybit.com");
    const auto res = http.get("/v5/market/instruments-info?category=" + cfg_.category + "&symbol=" + std::string(symbol));
    const auto spec = bybit::parse_instrument_info(res.body);
    if (!spec) {
        throw std::runtime_error("bybit: unknown symbol '" + std::string(symbol) + "' in category " + cfg_.category +
                                 " (HTTP " + std::to_string(res.status) + ")");
    }
    return {spec->tick_size, spec->lot_size, spec->min_qty};
}

void BybitVenue::connect() {
    Impl& im = *impl_;
    if (!sink_) throw std::logic_error("BybitVenue::connect: set_sink() was not called");
    if (im.thread.joinable()) return;

    // Wrong sizes do not fail loudly: every price would silently be rounded
    // to the wrong tick. Refuse to start instead.
    for (const auto& [id, inst] : instruments_) {
        const auto spec = fetch_instrument(inst.symbol);
        const auto same = [](double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(std::abs(a), std::abs(b)); };
        if (!same(inst.tick_size, spec.tick_size) || !same(inst.lot_size, spec.lot_size)) {
            char text[256];
            std::snprintf(text, sizeof(text), "%s has tick %g / lot %g on Bybit, but %g / %g were configured",
                          inst.symbol.c_str(), spec.tick_size, spec.lot_size, inst.tick_size, inst.lot_size);
            throw std::runtime_error(std::string("BybitVenue::connect: ") + text);
        }
    }

    for (const auto& [id, inst] : instruments_) im.symbols[inst.symbol] = &inst;

    const std::string ws_host = cfg_.testnet ? "wss://stream-testnet.bybit.com" : "wss://stream.bybit.com";
    im.pub.ws = std::make_unique<transport::WsClient>(
        im.io, ws_host + "/v5/public/" + cfg_.category,
        transport::WsClient::Handlers{
            .on_open = [&im] { im.on_public_open(); },
            .on_message = [&im](std::string_view msg) { im.on_public_message(msg); },
            .on_close = [&im](std::string_view reason) { im.on_closed(im.pub, reason); },
        },
        kIdleTimeout);

    if (im.has_keys()) {
        im.priv.ws = std::make_unique<transport::WsClient>(
            im.io, ws_host + "/v5/private",
            transport::WsClient::Handlers{
                .on_open = [&im] { im.on_private_open(); },
                .on_message = [&im](std::string_view msg) { im.on_private_message(msg); },
                .on_close = [&im](std::string_view reason) { im.on_closed(im.priv, reason); },
            },
            kIdleTimeout);
        im.trade.ws = std::make_unique<transport::WsClient>(
            im.io, ws_host + "/v5/trade",
            transport::WsClient::Handlers{
                .on_open = [&im] { im.on_trade_open(); },
                .on_message = [&im](std::string_view msg) { im.on_trade_message(msg); },
                .on_close = [&im](std::string_view reason) { im.on_closed(im.trade, reason); },
            },
            kIdleTimeout);
        im.rest = std::make_unique<transport::HttpClient>(cfg_.testnet ? "https://api-testnet.bybit.com"
                                                                       : "https://api.bybit.com");
    } else {
        PEEWEE_LOG("bybit: no API key, public data only (orders will be rejected)");
    }

    im.work.emplace(asio::make_work_guard(im.io));
    im.thread = std::thread([&im] { im.io.run(); });

    asio::post(im.io, [&im] {
        for (Impl::Conn* c : {&im.pub, &im.priv, &im.trade}) {
            if (c->ws) c->ws->connect();
        }
        im.schedule_ping();
    });

    // Block until everything is usable, so the first orders do not race the login.
    bool ok = false;
    std::string error;
    {
        std::unique_lock lock(im.m);
        ok = im.cv.wait_for(lock, kConnectTimeout, [&] {
            return !im.connect_error.empty() ||
                   (im.public_up && (!im.has_keys() || (im.private_up && im.trade_up)));
        });
        error = im.connect_error;
    }
    if (!ok || !error.empty()) {
        disconnect();
        throw std::runtime_error("BybitVenue::connect: " + (error.empty() ? std::string("timed out") : error));
    }
}

void BybitVenue::disconnect() {
    Impl& im = *impl_;
    if (!im.thread.joinable()) return;

    // REST first: its jobs post back to the network thread.
    im.rest_pool.stop();
    im.rest_pool.join();

    std::promise<void> closed;
    asio::post(im.io, [&im, &closed] {
        for (Impl::Conn* c : {&im.pub, &im.priv, &im.trade}) {
            c->retry.cancel();
            if (c->ws) c->ws->close();
        }
        im.ping_timer.cancel();
        closed.set_value();
    });
    closed.get_future().wait();

    im.work.reset();
    im.io.stop();
    im.thread.join();
}

void BybitVenue::subscribe_book(InstrumentId inst, int depth) {
    const auto it = instruments_.find(inst);
    if (it == instruments_.end()) {
        PEEWEE_LOG("bybit: subscribe_book for unknown instrument %u", inst);
        return;
    }
    // Available depths for linear: 1, 50, 200, 1000.
    int bybit_depth = 1000;
    for (int d : {1, 50, 200, 1000}) {
        if (d >= depth) {
            bybit_depth = d;
            break;
        }
    }
    std::string topic = "orderbook." + std::to_string(bybit_depth) + "." + it->second.symbol;

    Impl& im = *impl_;
    asio::post(im.io, [&im, topic = std::move(topic)] {
        im.book_topics.push_back(topic);
        if (im.pub.ready) im.subscribe(*im.pub.ws, {topic});
    });
}

void BybitVenue::subscribe_private() {
    Impl& im = *impl_;
    if (!im.has_keys()) return;
    asio::post(im.io, [&im] {
        im.private_wanted = true;
        if (im.priv.ready && !im.private_subscribed) im.subscribe_private_topics();
    });
}

void BybitVenue::send_new(const NewOrder& o) {
    Impl& im = *impl_;
    im.buckets[static_cast<int>(RequestKind::New)].try_acquire(steady_now());
    const Instrument* inst = &instruments_.at(o.inst);
    asio::post(im.io, [this, &im, o, inst] {
        if (!im.trade.ready) return im.emit_reject(o.cid, o.req, RequestKind::New, "trade connection down");
        im.send_trade(bybit::make_create(o, *inst, cfg_.category, wall_ms()));
    });
}

void BybitVenue::send_amend(const AmendOrder& o) {
    Impl& im = *impl_;
    im.buckets[static_cast<int>(RequestKind::Amend)].try_acquire(steady_now());
    const Instrument* inst = &instruments_.at(o.inst);
    asio::post(im.io, [this, &im, o, inst] {
        if (!im.trade.ready) return im.emit_reject(o.cid, o.req, RequestKind::Amend, "trade connection down");
        im.send_trade(bybit::make_amend(o, *inst, cfg_.category, wall_ms()));
    });
}

void BybitVenue::send_cancel(const CancelOrder& o) {
    Impl& im = *impl_;
    im.buckets[static_cast<int>(RequestKind::Cancel)].try_acquire(steady_now());
    const Instrument* inst = &instruments_.at(o.inst);
    asio::post(im.io, [this, &im, o, inst] {
        if (!im.trade.ready) return im.emit_reject(o.cid, o.req, RequestKind::Cancel, "trade connection down");
        im.send_trade(bybit::make_cancel(o, *inst, cfg_.category, wall_ms()));
    });
}

void BybitVenue::query_order(const QueryOrder& q) {
    Impl& im = *impl_;
    if (!im.rest) return;
    const auto it = instruments_.find(q.inst);
    if (it == instruments_.end()) return;

    // REST GET /v5/order/realtime: by orderLinkId it also returns recently
    // closed orders. Older ones would need /v5/order/history (not done yet).
    std::string query = "category=" + cfg_.category + "&symbol=" + it->second.symbol +
                        "&orderLinkId=" + std::to_string(q.cid);
    asio::post(im.rest_pool, [this, &im, cid = q.cid, query = std::move(query)] {
        try {
            const int64_t ts = wall_ms();
            const transport::HttpHeaders headers = {
                {"X-BAPI-API-KEY", cfg_.api_key},
                {"X-BAPI-TIMESTAMP", std::to_string(ts)},
                {"X-BAPI-RECV-WINDOW", std::to_string(kRecvWindowMs)},
                {"X-BAPI-SIGN", bybit::rest_signature(cfg_.api_secret, cfg_.api_key, ts, kRecvWindowMs, query)},
            };
            const auto res = im.rest->get("/v5/order/realtime?" + query, headers);
            auto status = bybit::parse_order_status(res.body, im.symbols, cid);
            if (!status) {
                // No event: the OMS queries again after its timeout.
                PEEWEE_LOG("bybit: order query for cid=%llu failed (HTTP %d): %s",
                           static_cast<unsigned long long>(cid), res.status, res.body.c_str());
                return;
            }
            asio::post(im.io, [&im, status = *status] { im.emit(OrderEvent{status}); });
        } catch (const std::exception& e) {
            PEEWEE_LOG("bybit: order query for cid=%llu failed: %s", static_cast<unsigned long long>(cid), e.what());
        }
    });
}

void BybitVenue::resync() {
    // The engine lost events. Reconnecting the public stream brings fresh
    // snapshots (and Public-up); Private-up makes the OMS reconcile orders.
    Impl& im = *impl_;
    asio::post(im.io, [&im] {
        im.pub.ready = false;
        im.pub.ws->close();
        im.pub.ws->connect();
        if (im.private_subscribed) im.emit_status(StreamKind::Private, true);
    });
}

bool BybitVenue::can_send(RequestKind kind) const {
    return impl_->buckets[static_cast<int>(kind)].available(steady_now());
}

}  // namespace peewee
