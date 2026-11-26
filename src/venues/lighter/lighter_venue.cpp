#include "peewee/venues/lighter.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <future>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "lighter_codec.hpp"
#include "lighter_signer.hpp"
#include "peewee/core/log.hpp"
#include "transport/http_client.hpp"
#include "transport/ws_client.hpp"

namespace peewee {

namespace asio = boost::asio;

// Endpoints (mainnet / testnet), https://docs.lighter.xyz/developers/getting-started/environments:
//   WS  : wss://mainnet.zklighter.elliot.ai/stream   | wss://api.testnet.lighter.xyz/stream
//   REST: https://mainnet.zklighter.elliot.ai/api/v1 | https://api.testnet.lighter.xyz/api/v1
// One WebSocket carries books, private events and orders ("jsonapi/sendtx").

namespace {

constexpr auto kConnectTimeout = std::chrono::seconds(15);
// Lighter closes connections silent for 2 minutes. A {"type":"ping"} every
// 10 s makes it answer {"type":"pong"}: 25 s without anything = dead
// connection. (Pings count toward the 200 messages/minute limit: 6/minute.)
constexpr auto kPingInterval = std::chrono::seconds(10);
constexpr auto kIdleTimeout = std::chrono::seconds(25);
constexpr int kMaxBackoffSeconds = 30;

// A transaction is refused after this (lighter-go's default, minus a margin).
constexpr int64_t kTxLifetimeMs = 10 * 60 * 1000 - 1000;
// Resting orders: Lighter has no GTC, only good-till-time up to 30 days.
// 28 days, as lighter-go uses for "no expiry".
constexpr int64_t kOrderLifetimeMs = int64_t{28} * 24 * 60 * 60 * 1000;
// Auth tokens may live at most 8 hours.
constexpr int64_t kAuthLifetimeS = 7 * 60 * 60;
// A refused transaction is answered within milliseconds: after this, a sent
// transaction is taken as accepted and forgotten.
constexpr int64_t kSentKeepMs = 60 * 1000;
// accountOrders (an indexer) may not list an order this young yet.
constexpr int64_t kIndexerLagMs = 5 * 1000;
// Orders remembered for amends and event mapping.
constexpr size_t kMaxOrders = 10000;

Timestamp wall_now() {
    return std::chrono::duration_cast<Timestamp>(std::chrono::system_clock::now().time_since_epoch());
}

int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t wall_s() { return wall_ms() / 1000; }

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string rest_url(bool testnet) {
    return testnet ? "https://api.testnet.lighter.xyz" : "https://mainnet.zklighter.elliot.ai";
}

// A price in ticks as Lighter's uint32 price, or 0 if it does not fit.
uint32_t wire_price(Price px) {
    return px.ticks > 0 && px.ticks <= std::numeric_limits<uint32_t>::max() ? static_cast<uint32_t>(px.ticks) : 0;
}

int64_t client_order_index(ClientOrderId cid) {
    return static_cast<int64_t>(cid & static_cast<uint64_t>(lighter::kMaxClientOrderIndex));
}

std::string book_channel(int64_t market) { return "order_book/" + std::to_string(market); }

const bool kDebug = std::getenv("PEEWEE_DEBUG") != nullptr;

}  // namespace

// THREADING: every WebSocket callback, timer and on_event() call runs on
// thread_ (the single network thread). Engine-thread calls are posted to it
// with asio::post. Blocking REST calls (query_order, nonce refresh) run on
// rest_pool, and post their results back to the network thread.
struct LighterVenue::Net {
    explicit Net(asio::io_context& io) : work(asio::make_work_guard(io)), ping_timer(io), retry_timer(io) {}

    asio::executor_work_guard<asio::io_context::executor_type> work;
    asio::steady_timer ping_timer;
    asio::steady_timer retry_timer;
    int backoff_s = 1;
    bool open = false;
    bool opened_before = false;  // A reconnect, not the first connection.

    // Read-only once the network thread runs (also read on the REST thread).
    lighter::MarketMap markets;  // market_id -> instrument, for the codec.

    std::vector<int64_t> book_markets;  // Markets to (re)subscribe.
    // Last book nonce per market; 0 = waiting for a snapshot (deltas dropped).
    std::unordered_map<int64_t, uint64_t> book_nonces;
    bool books_stale = false;  // Public is down until every market has a new snapshot.
    bool private_wanted = false;

    // Transactions sent, by request id, until Lighter refuses them or they
    // are old enough that no answer will come (successes are not parsed:
    // their format was not captured yet, see tests/data/lighter/README.md).
    struct Sent {
        ClientOrderId cid;
        RequestKind kind;
        TxSigner sign;
        int64_t at_ms;
        bool retried = false;  // Re-signed once after a nonce refusal.
    };
    std::unordered_map<RequestId, Sent> sent;
    std::deque<RequestId> sent_fifo;  // Insertion order, for pruning.

    bool nonce_refreshing = false;
    std::vector<RequestId> nonce_retries;  // Refused for their nonce: re-signed after the refresh.

    asio::thread_pool rest_pool{1};

    // connect() waits for the first on_open.
    std::mutex m;
    std::condition_variable cv;
    bool up = false;
};

LighterVenue::LighterVenue(VenueId id, LighterConfig cfg) : id_(id), cfg_(std::move(cfg)) {
    caps_.native_amend = true;          // ModifyOrder transaction.
    caps_.amend_keeps_priority = false;
    caps_.ws_order_entry = true;        // "jsonapi/sendtx" over WebSocket.
    caps_.post_only = true;
    caps_.book_checksum = false;        // Continuity is checked with nonces instead.
    caps_.max_book_depth = 0;           // The whole book: depth is not selectable.
}

LighterVenue::~LighterVenue() { disconnect(); }

void LighterVenue::add_instrument(const Instrument& inst) { instruments_[inst.id] = inst; }

const std::string& LighterVenue::market_details() const {
    // One download for fetch_instrument() of every symbol and connect()'s check.
    if (market_details_.empty()) {
        transport::HttpClient http(rest_url(cfg_.testnet));
        const auto res = http.get("/api/v1/orderBookDetails");
        if (!lighter::parse_market_details(res.body)) {
            throw std::runtime_error("LighterVenue: unreadable orderBookDetails (HTTP " +
                                     std::to_string(res.status) + ")");
        }
        market_details_ = res.body;
    }
    return market_details_;
}

LighterInstrumentSpec LighterVenue::fetch_instrument(std::string_view symbol) const {
    const auto markets = lighter::parse_market_details(market_details());
    const auto it = markets->find(std::string(symbol));
    if (it == markets->end()) {
        throw std::runtime_error("LighterVenue::fetch_instrument: unknown market '" + std::string(symbol) + "'");
    }
    return LighterInstrumentSpec{.tick_size = std::pow(10.0, -it->second.price_decimals),
                                 .lot_size = std::pow(10.0, -it->second.size_decimals),
                                 .min_qty = it->second.min_base_amount};
}

void LighterVenue::load_markets() {
    const auto markets = lighter::parse_market_details(market_details());
    for (const auto& [id, inst] : instruments_) {
        const auto it = markets->find(inst.symbol);
        if (it == markets->end()) {
            throw std::runtime_error("LighterVenue::connect: unknown market '" + inst.symbol + "'");
        }
        // Prices and sizes are sent as integers in these decimals: wrong sizes
        // would send every price and quantity off by a power of ten.
        const double tick = std::pow(10.0, -it->second.price_decimals);
        const double lot = std::pow(10.0, -it->second.size_decimals);
        if (std::abs(inst.tick_size - tick) > 1e-9 * tick || std::abs(inst.lot_size - lot) > 1e-9 * lot) {
            char text[256];
            std::snprintf(text, sizeof(text), "%s has tick %g and lot %g on Lighter, but %g and %g were configured",
                          inst.symbol.c_str(), tick, lot, inst.tick_size, inst.lot_size);
            throw std::runtime_error(std::string("LighterVenue::connect: ") + text);
        }
        market_ids_[id] = it->second.market_id;
    }
}

void LighterVenue::check_api_key() {
    // Ask Lighter which public key is registered at (account, key index):
    // catches a wrong key or index before any order is signed.
    transport::HttpClient http(rest_url(cfg_.testnet));
    const std::string query = "?account_index=" + std::to_string(cfg_.account_index) +
                              "&api_key_index=" + std::to_string(cfg_.api_key_index);
    const auto res = http.get("/api/v1/apikeys" + query);
    const auto registered = lighter::parse_api_key(res.body);
    if (!registered) {
        throw std::runtime_error("LighterVenue::connect: no API key " + std::to_string(cfg_.api_key_index) +
                                 " on account " + std::to_string(cfg_.account_index) + " (answer: " + res.body + ")");
    }
    std::string expected = lowercase(*registered);
    if (expected.starts_with("0x")) expected.erase(0, 2);
    if (expected != signer_->public_key_hex()) {
        throw std::runtime_error("LighterVenue::connect: the private key does not match API key " +
                                 std::to_string(cfg_.api_key_index) + " of account " +
                                 std::to_string(cfg_.account_index));
    }

    const auto nonce_res = http.get("/api/v1/nextNonce" + query);
    const auto nonce = lighter::parse_next_nonce(nonce_res.body);
    if (!nonce) {
        throw std::runtime_error("LighterVenue::connect: unreadable nextNonce (answer: " + nonce_res.body + ")");
    }
    nonce_.store(*nonce);
    PEEWEE_LOG("lighter: account %lld, API key %d, next nonce %lld", static_cast<long long>(cfg_.account_index),
               cfg_.api_key_index, static_cast<long long>(*nonce));
}

void LighterVenue::connect() {
    if (!sink_) throw std::logic_error("LighterVenue::connect: set_sink() was not called");
    if (thread_.joinable()) return;  // Already connected.
    // The engine accepts events from one thread per venue for its whole life:
    // a new network thread would abort it. Make a new venue instead.
    if (io_) throw std::logic_error("LighterVenue::connect: cannot connect again after disconnect()");

    load_markets();
    if (!cfg_.api_private_key.empty()) {
        if (cfg_.account_index < 0 || cfg_.api_key_index < 0 || cfg_.api_key_index > 254) {
            throw std::invalid_argument("LighterVenue::connect: set account_index and api_key_index (0-254)");
        }
        signer_ = std::make_unique<lighter::Signer>(
            cfg_.api_private_key, cfg_.testnet ? lighter::kTestnetChainId : lighter::kMainnetChainId,
            cfg_.account_index, static_cast<uint8_t>(cfg_.api_key_index));
        check_api_key();
    } else {
        PEEWEE_LOG("lighter: no API key, public data only (orders will be rejected)");
    }

    io_ = std::make_unique<asio::io_context>();
    net_ = std::make_unique<Net>(*io_);
    for (const auto& [id, market] : market_ids_) net_->markets.insert_or_assign(market, &instruments_.at(id));
    const std::string url = cfg_.testnet ? "wss://api.testnet.lighter.xyz/stream"
                                         : "wss://mainnet.zklighter.elliot.ai/stream";
    ws_ = std::make_unique<transport::WsClient>(
        *io_, url,
        transport::WsClient::Handlers{
            .on_open = [this] { on_open(); },
            .on_message = [this](std::string_view msg) { on_message(msg); },
            .on_close = [this](std::string_view reason) { on_close(reason); },
        },
        kIdleTimeout);

    thread_ = std::thread([this] { io_->run(); });
    asio::post(*io_, [this] {
        ws_->connect();
        schedule_ping();
    });

    bool ok = false;
    {
        std::unique_lock lock(net_->m);
        ok = net_->cv.wait_for(lock, kConnectTimeout, [this] { return net_->up; });
    }
    if (!ok) {
        disconnect();
        throw std::runtime_error("LighterVenue::connect: timed out");
    }
}

void LighterVenue::disconnect() {
    if (!thread_.joinable()) return;

    // Close first: once closed, no message can start another REST call.
    std::promise<void> closed;
    asio::post(*io_, [this, &closed] {
        net_->open = false;
        net_->retry_timer.cancel();
        net_->ping_timer.cancel();
        ws_->close();
        closed.set_value();
    });
    closed.get_future().wait();

    // Then wait for REST calls in progress: they post their answers to the
    // network thread, which still runs.
    net_->rest_pool.join();

    net_->work.reset();
    io_->stop();
    thread_.join();
}

// --- Network thread ---

void LighterVenue::on_open() {
    net_->backoff_s = 1;
    net_->open = true;
    subscribe_books(/*resubscribe=*/false);  // A new connection has no subscriptions.
    if (signer_) sink_->on_event(VenueStatus{id_, StreamKind::Trading, true, wall_now()});
    if (net_->private_wanted) send_private_subscriptions();
    // Transactions in flight when the connection dropped may or may not have
    // reached Lighter: the nonce is unknown.
    if (signer_ && net_->opened_before) refresh_nonce();
    net_->opened_before = true;

    {
        std::lock_guard lock(net_->m);
        net_->up = true;
    }
    net_->cv.notify_all();
}

void LighterVenue::subscribe_books(bool resubscribe) {
    // On an open connection, the only way to a new snapshot is to unsubscribe
    // and subscribe again (seen on testnet 2026-11-26: a second subscribe is
    // refused with 30003 "Already Subscribed", an unsubscribe of a channel not
    // subscribed with 30002).
    net_->books_stale = true;
    for (int64_t market : net_->book_markets) {
        net_->book_nonces[market] = 0;
        if (resubscribe) ws_->send(lighter::make_unsubscribe(book_channel(market)));
        ws_->send(lighter::make_subscribe(book_channel(market)));
    }
    public_up_when_ready();  // At once if there is no book.
}

void LighterVenue::public_up_when_ready() {
    if (!net_->books_stale) return;
    for (int64_t market : net_->book_markets) {
        if (net_->book_nonces[market] == 0) return;
    }
    net_->books_stale = false;
    sink_->on_event(VenueStatus{id_, StreamKind::Public, true, wall_now()});
}

void LighterVenue::send_private_subscriptions() {
    // A fresh token each time: they expire, and a reconnect needs a valid one.
    const std::string token = signer_->auth_token(wall_s() + kAuthLifetimeS);
    const std::string account = std::to_string(cfg_.account_index);
    ws_->send(lighter::make_subscribe("account_all_orders/" + account, token));
    ws_->send(lighter::make_subscribe("account_all_trades/" + account, token));
    sink_->on_event(VenueStatus{id_, StreamKind::Private, true, wall_now()});
}

void LighterVenue::forward(OrderEvent&& e) {
    // Lighter echoes client_order_index; ours is the full cid. Unknown indexes
    // are orders this process did not send (e.g. from the website): skipped.
    const int64_t coi = static_cast<int64_t>(cid_of(e));
    {
        std::lock_guard lock(orders_m_);
        const auto it = orders_.find(coi);
        if (it == orders_.end()) return;
        OrderInfo& info = it->second;
        std::visit([&info](auto& x) { x.cid = info.cid; }, e);

        if (const auto* ack = std::get_if<OrderAck>(&e)) {
            info.live = true;
            info.qty = ack->qty.lots;  // The size a modify must keep.
        } else if (const auto* r = std::get_if<OrderReject>(&e);
                   r && info.live && r->reason == RejectReason::PostOnlyWouldCross) {
            // canceled-post-only of an order that was already open: a modify
            // made it cross, and Lighter canceled it.
            e = OrderClosed{.cid = info.cid, .req = kNoRequest, .reason = CloseReason::Canceled, .ts = r->ts};
        }
    }
    sink_->on_event(std::move(e));
}

void LighterVenue::emit_reject(ClientOrderId cid, RequestId req, RequestKind kind, RejectReason reason,
                               std::string text) {
    OrderReject r;
    r.cid = cid;
    r.req = req;
    r.request = kind;
    r.reason = reason;
    r.text = std::move(text);
    r.ts = wall_now();
    sink_->on_event(OrderEvent{std::move(r)});
}

void LighterVenue::on_message(std::string_view msg) {
    // PEEWEE_DEBUG=1 prints every message received, except books (too many) and pongs.
    if (kDebug && !msg.starts_with(R"({"channel":"order_book:)") && msg != R"({"type":"pong"})") {
        PEEWEE_LOG("lighter <- %.*s", static_cast<int>(msg.size()), msg.data());
    }

    if (msg.starts_with(R"({"channel":"order_book:)")) {
        auto update = lighter::parse_order_book(msg, net_->markets);
        if (!update) return;
        const int64_t market = market_ids_.at(update->delta.inst);
        uint64_t& last = net_->book_nonces[market];
        if (!update->delta.snapshot) {
            if (last == 0) return;  // Waiting for the snapshot of a new subscription.
            if (update->begin_nonce != last) {
                // A lost update: the book is wrong until a new snapshot. Books
                // are invalidated per venue, so every market starts over.
                PEEWEE_LOG("lighter: gap in market %lld (nonce %llu, expected %llu): resubscribing",
                           static_cast<long long>(market), static_cast<unsigned long long>(update->begin_nonce),
                           static_cast<unsigned long long>(last));
                sink_->on_event(VenueStatus{id_, StreamKind::Public, false, wall_now()});
                subscribe_books(/*resubscribe=*/true);  // Public up again with the snapshots.
                return;
            }
        }
        last = update->delta.seq;
        const bool snapshot = update->delta.snapshot;
        sink_->on_event(std::move(update->delta));
        if (snapshot) public_up_when_ready();
        return;
    }
    if (msg.find(R"("channel":"account_all_orders:)") != std::string_view::npos) {
        for (auto& event : lighter::parse_account_orders(msg, net_->markets)) forward(std::move(event));
        return;
    }
    if (msg.find(R"("channel":"account_all_trades:)") != std::string_view::npos) {
        for (auto& event : lighter::parse_account_trades(msg, net_->markets, cfg_.account_index)) {
            forward(std::move(event));
        }
        return;
    }
    if (msg.find(R"("type":"ping")") != std::string_view::npos) {  // The server's own keepalive.
        ws_->send(R"({"type":"pong"})");
        return;
    }
    // "connected", "pong", subscription and sendtx acknowledgements: nothing
    // to do (an accepted tx is confirmed by account_all_orders).
    if (msg.find(R"("error")") == std::string_view::npos) return;
    const auto err = lighter::parse_error(msg);
    if (!err) return;
    // The answer to a refused transaction carries our request number as id.
    const auto it = err->id.empty() ? net_->sent.end() : net_->sent.find(std::strtoull(err->id.c_str(), nullptr, 10));
    if (it == net_->sent.end()) {
        PEEWEE_LOG("lighter: error %d: %s", err->code, err->text.c_str());
        return;
    }
    if (lighter::is_nonce_error(err->code) && !it->second.retried) {
        // Most likely a transaction sent after one that was refused (a
        // refused transaction does not use its nonce): sign it again with
        // the nonce Lighter expects, once.
        PEEWEE_LOG("lighter: request %llu refused for its nonce (%d), re-signing",
                   static_cast<unsigned long long>(it->first), err->code);
        it->second.retried = true;
        net_->nonce_retries.push_back(it->first);
        refresh_nonce();
        return;
    }
    const RequestId req = it->first;
    const Net::Sent sent = std::move(it->second);
    net_->sent.erase(it);
    // The transactions sent after this one will be refused too: re-read the nonce.
    refresh_nonce();
    emit_reject(sent.cid, req, sent.kind, lighter::reject_reason_from_code(err->code),
                std::to_string(err->code) + " " + err->text);
}

void LighterVenue::on_close(std::string_view reason) {
    PEEWEE_LOG("lighter: connection lost (%.*s), reconnecting in %d s", static_cast<int>(reason.size()),
               reason.data(), net_->backoff_s);
    net_->open = false;
    sink_->on_event(VenueStatus{id_, StreamKind::Public, false, wall_now()});
    if (signer_) sink_->on_event(VenueStatus{id_, StreamKind::Trading, false, wall_now()});
    if (net_->private_wanted) sink_->on_event(VenueStatus{id_, StreamKind::Private, false, wall_now()});

    net_->retry_timer.expires_after(std::chrono::seconds(net_->backoff_s));
    net_->retry_timer.async_wait([this](const boost::system::error_code& ec) {
        if (!ec) ws_->connect();
    });
    net_->backoff_s = std::min(net_->backoff_s * 2, kMaxBackoffSeconds);
}

void LighterVenue::schedule_ping() {
    net_->ping_timer.expires_after(kPingInterval);
    net_->ping_timer.async_wait([this](const boost::system::error_code& ec) {
        if (ec) return;  // Cancelled by disconnect().
        if (ws_->is_open()) ws_->send(R"({"type":"ping"})");
        schedule_ping();
    });
}

void LighterVenue::refresh_nonce() {
    if (net_->nonce_refreshing) return;  // One refresh at a time; its answer serves every retry.
    net_->nonce_refreshing = true;
    asio::post(net_->rest_pool, [this] {
        std::optional<int64_t> nonce;
        try {
            transport::HttpClient http(rest_url(cfg_.testnet));
            const auto res = http.get("/api/v1/nextNonce?account_index=" + std::to_string(cfg_.account_index) +
                                      "&api_key_index=" + std::to_string(cfg_.api_key_index));
            nonce = lighter::parse_next_nonce(res.body);
            if (!nonce) PEEWEE_LOG("lighter: nonce refresh failed (HTTP %d): %.200s", res.status, res.body.c_str());
        } catch (const std::exception& e) {
            PEEWEE_LOG("lighter: nonce refresh failed: %s", e.what());
        }
        asio::post(*io_, [this, nonce] { on_nonce(nonce); });
    });
}

void LighterVenue::on_nonce(std::optional<int64_t> nonce) {
    // ponytail: the engine may sign with the old counter while this runs, and
    // nextNonce may not count transactions Lighter has not processed yet: a
    // nonce can still be reused. Each request is re-signed once, then rejected.
    net_->nonce_refreshing = false;
    if (nonce) {
        nonce_.store(*nonce);
        PEEWEE_LOG("lighter: nonce re-read: %lld", static_cast<long long>(*nonce));
    }
    for (RequestId req : std::exchange(net_->nonce_retries, {})) {
        const auto it = net_->sent.find(req);
        if (it == net_->sent.end()) continue;
        if (nonce && net_->open) {
            std::string message = it->second.sign(next_nonce());
            if (kDebug) PEEWEE_LOG("lighter -> %s", message.c_str());
            ws_->send(message);
            continue;
        }
        const Net::Sent sent = std::move(it->second);
        net_->sent.erase(it);
        emit_reject(sent.cid, req, sent.kind, RejectReason::VenueUnavailable,
                    nonce ? "not connected" : "invalid nonce, and nextNonce could not be read");
    }
}

void LighterVenue::prune_sent() {
    // Refusals come back within milliseconds: an entry this old was accepted.
    const int64_t oldest = wall_ms() - kSentKeepMs;
    while (!net_->sent_fifo.empty()) {
        const auto it = net_->sent.find(net_->sent_fifo.front());
        if (it != net_->sent.end() && it->second.at_ms > oldest) break;
        if (it != net_->sent.end()) net_->sent.erase(it);
        net_->sent_fifo.pop_front();
    }
}

// --- Engine thread ---

void LighterVenue::subscribe_book(InstrumentId inst, int /*depth*/) {
    // depth is ignored: Lighter always sends the whole book.
    const auto it = market_ids_.find(inst);
    if (it == market_ids_.end()) {
        PEEWEE_LOG("lighter: subscribe_book for unknown instrument %u (or before connect())", inst);
        return;
    }
    asio::post(*io_, [this, market = it->second] {
        auto& markets = net_->book_markets;
        if (std::find(markets.begin(), markets.end(), market) != markets.end()) return;  // Already subscribed.
        markets.push_back(market);
        net_->book_nonces[market] = 0;
        if (net_->open) ws_->send(lighter::make_subscribe(book_channel(market)));
    });
}

void LighterVenue::subscribe_private() {
    if (!signer_) return;  // Public data only: private channels need an auth token.
    if (!thread_.joinable()) {
        PEEWEE_LOG("lighter: subscribe_private before connect()");
        return;
    }
    asio::post(*io_, [this] {
        if (net_->private_wanted) return;
        net_->private_wanted = true;
        if (net_->open) send_private_subscriptions();
    });
}

int64_t LighterVenue::next_nonce() { return nonce_.fetch_add(1); }

void LighterVenue::reject_later(ClientOrderId cid, RequestId req, RequestKind kind, RejectReason reason,
                                std::string text) {
    PEEWEE_LOG("lighter: %s request %llu not sent: %s", to_string(kind), static_cast<unsigned long long>(req),
               text.c_str());
    if (!thread_.joinable()) return;
    asio::post(*io_, [this, cid, req, kind, reason, text = std::move(text)]() mutable {
        emit_reject(cid, req, kind, reason, std::move(text));
    });
}

void LighterVenue::send_tx(ClientOrderId cid, RequestId req, RequestKind kind, TxSigner sign) {
    std::string message = sign(next_nonce());
    if (kDebug) PEEWEE_LOG("lighter -> %s", message.c_str());  // No secret in it: only the signature.
    asio::post(*io_, [this, cid, req, kind, sign = std::move(sign), message = std::move(message)]() mutable {
        if (!net_->open) {
            // The nonce was taken but never used: re-read it, or every later
            // transaction would be refused.
            refresh_nonce();
            emit_reject(cid, req, kind, RejectReason::VenueUnavailable, "not connected");
            return;
        }
        prune_sent();
        net_->sent.insert_or_assign(req, Net::Sent{cid, kind, std::move(sign), wall_ms()});
        net_->sent_fifo.push_back(req);
        ws_->send(message);
    });
}

void LighterVenue::remember_order(const NewOrder& o, int64_t coi) {
    // ponytail: the oldest orders are forgotten past kMaxOrders; an amend of
    // one is then rejected (OrderNotFound: the OMS queries it) and its events
    // are skipped. Raise the limit if that many orders live at once.
    std::lock_guard lock(orders_m_);
    orders_.insert_or_assign(coi, OrderInfo{.cid = o.cid,
                                            .inst = o.inst,
                                            .trigger_px = is_trigger(o.type) ? wire_price(o.trigger_px) : 0,
                                            .qty = o.qty.lots,
                                            .sent_ms = wall_ms()});
    order_fifo_.push_back(coi);
    while (order_fifo_.size() > kMaxOrders) {
        orders_.erase(order_fifo_.front());
        order_fifo_.pop_front();
    }
}

void LighterVenue::send_new(const NewOrder& o) {
    const auto market_it = market_ids_.find(o.inst);
    if (!signer_ || !thread_.joinable() || market_it == market_ids_.end()) {
        reject_later(o.cid, o.req, RequestKind::New, RejectReason::VenueUnavailable,
                     signer_ ? "unknown instrument or not connected" : "no API key");
        return;
    }
    const auto spec = lighter::order_spec(o.type, o.tif);
    if (!spec) {
        reject_later(o.cid, o.req, RequestKind::New, RejectReason::Unknown,
                     std::string(to_string(o.type)) + " with this tif is not possible on Lighter");
        return;
    }
    const uint32_t px = wire_price(o.px);
    const uint32_t trigger = is_trigger(o.type) ? wire_price(o.trigger_px) : 0;
    if (px == 0 || (is_trigger(o.type) && trigger == 0)) {
        reject_later(o.cid, o.req, RequestKind::New, RejectReason::InvalidPrice, "price out of Lighter's range");
        return;
    }
    if (o.qty.lots <= 0) {
        reject_later(o.cid, o.req, RequestKind::New, RejectReason::InvalidQty, "quantity must be positive");
        return;
    }

    const int64_t coi = client_order_index(o.cid);
    remember_order(o, coi);
    const int64_t now = wall_ms();
    const lighter::CreateOrderTx tx{
        .market_index = market_it->second,
        .client_order_index = coi,
        .base_amount = o.qty.lots,
        .price = px,
        .is_ask = o.side == Side::Sell ? uint8_t{1} : uint8_t{0},
        .type = spec->type,
        .time_in_force = spec->time_in_force,
        .reduce_only = o.reduce_only ? uint8_t{1} : uint8_t{0},
        .trigger_price = trigger,
        .order_expiry = spec->expires ? now + kOrderLifetimeMs : 0,
    };
    send_tx(o.cid, o.req, RequestKind::New, [this, req = o.req, tx, expired = now + kTxLifetimeMs](int64_t nonce) {
        return lighter::make_send_tx(req, lighter::kTxCreateOrder,
                                     signer_->sign_create_order(tx, {.expired_at = expired, .nonce = nonce}));
    });
}

void LighterVenue::send_amend(const AmendOrder& o) {
    const int64_t coi = client_order_index(o.cid);
    std::optional<OrderInfo> order;
    {
        std::lock_guard lock(orders_m_);
        if (const auto it = orders_.find(coi); it != orders_.end()) order = it->second;
    }
    if (!signer_ || !thread_.joinable() || !order) {
        reject_later(o.cid, o.req, RequestKind::Amend, RejectReason::OrderNotFound, "order unknown to this venue");
        return;
    }
    const uint32_t px = wire_price(o.px);
    if (px == 0) {
        reject_later(o.cid, o.req, RequestKind::Amend, RejectReason::InvalidPrice, "price out of Lighter's range");
        return;
    }
    // Modify sets the order's total size (filled part included): without a
    // new one, keep the size Lighter last confirmed, not one still in flight.
    const lighter::ModifyOrderTx tx{
        .market_index = market_ids_.at(order->inst),
        .index = coi,
        .base_amount = o.qty ? o.qty->lots : order->qty,
        .price = px,
        .trigger_price = order->trigger_px,
    };
    send_tx(o.cid, o.req, RequestKind::Amend,
            [this, req = o.req, tx, expired = wall_ms() + kTxLifetimeMs](int64_t nonce) {
                return lighter::make_send_tx(req, lighter::kTxModifyOrder,
                                             signer_->sign_modify_order(tx, {.expired_at = expired, .nonce = nonce}));
            });
}

void LighterVenue::send_cancel(const CancelOrder& o) {
    const auto market_it = market_ids_.find(o.inst);
    if (!signer_ || !thread_.joinable() || market_it == market_ids_.end()) {
        reject_later(o.cid, o.req, RequestKind::Cancel, RejectReason::VenueUnavailable,
                     signer_ ? "unknown instrument or not connected" : "no API key");
        return;
    }
    const lighter::CancelOrderTx tx{.market_index = market_it->second, .index = client_order_index(o.cid)};
    send_tx(o.cid, o.req, RequestKind::Cancel,
            [this, req = o.req, tx, expired = wall_ms() + kTxLifetimeMs](int64_t nonce) {
                return lighter::make_send_tx(req, lighter::kTxCancelOrder,
                                             signer_->sign_cancel_order(tx, {.expired_at = expired, .nonce = nonce}));
            });
}

void LighterVenue::query_order(const QueryOrder& q) {
    // GET /api/v1/accountOrders by client_order_index (authenticated). It
    // covers open orders and the inactive ones of the last 24 hours.
    if (!thread_.joinable() || !signer_) return;
    asio::post(net_->rest_pool, [this, cid = q.cid] {
        const int64_t coi = client_order_index(cid);
        try {
            transport::HttpClient http(rest_url(cfg_.testnet));
            // The signer is only read here (sign() is const and thread-safe).
            const std::string token = signer_->auth_token(wall_s() + 600);
            const auto res = http.get("/api/v1/accountOrders?account_index=" + std::to_string(cfg_.account_index) +
                                          "&client_order_indexes=" + std::to_string(coi),
                                      {{"Authorization", token}});
            auto status = lighter::parse_order_query(res.body, net_->markets, coi);
            if (!status) {
                // No event: the OMS asks again after its timeout.
                PEEWEE_LOG("lighter: order query for cid=%llu failed (HTTP %d): %.200s",
                           static_cast<unsigned long long>(cid), res.status, res.body.c_str());
                return;
            }
            if (!status->found) {
                // accountOrders is an indexer behind the exchange: a recent
                // order may be live and not listed yet. "Not found" would make
                // the OMS forget it, so give no answer (the OMS asks again).
                int64_t sent_ms = 0;
                {
                    std::lock_guard lock(orders_m_);
                    if (const auto it = orders_.find(coi); it != orders_.end()) sent_ms = it->second.sent_ms;
                }
                if (wall_ms() - sent_ms < kIndexerLagMs) {
                    PEEWEE_LOG("lighter: order query cid=%llu: not listed yet, asking again later",
                               static_cast<unsigned long long>(cid));
                    return;
                }
            }
            status->cid = cid;
            if (!status->ts.count()) status->ts = wall_now();
            if (kDebug) {
                PEEWEE_LOG("lighter: order query cid=%llu: %s, %s", static_cast<unsigned long long>(cid),
                           status->found ? "found" : "NOT found", status->open ? "open" : "closed");
            }
            asio::post(*io_, [this, s = *status] { sink_->on_event(OrderEvent{s}); });
        } catch (const std::exception& e) {
            PEEWEE_LOG("lighter: order query for cid=%llu failed: %s", static_cast<unsigned long long>(cid), e.what());
        }
    });
}

void LighterVenue::resync() {
    // The engine lost events (it already reported both streams down): fresh
    // book snapshots (Public up once they arrive), and Private-up makes the
    // OMS query every open order.
    if (!thread_.joinable()) return;
    asio::post(*io_, [this] {
        if (!net_->open) return;
        subscribe_books(/*resubscribe=*/true);
        if (net_->private_wanted) sink_->on_event(VenueStatus{id_, StreamKind::Private, true, wall_now()});
    });
}

bool LighterVenue::can_send(RequestKind /*kind*/) const {
    // TODO: Lighter's sendTx limits depend on the account tier.
    return true;
}

}  // namespace peewee
