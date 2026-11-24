#include "peewee/venues/hyperliquid.hpp"

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
#include <future>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "hl_codec.hpp"
#include "hl_signer.hpp"
#include "peewee/core/log.hpp"
#include "transport/http_client.hpp"
#include "transport/ws_client.hpp"

namespace peewee {

namespace asio = boost::asio;

// Endpoint (mainnet / testnet):
//   WS  : wss://api.hyperliquid.xyz/ws         | wss://api.hyperliquid-testnet.xyz/ws
//   REST: https://api.hyperliquid.xyz/info     | https://api.hyperliquid-testnet.xyz/info
//         https://api.hyperliquid.xyz/exchange (signed actions, alternative to WS "post")

namespace {

constexpr auto kConnectTimeout = std::chrono::seconds(15);
// Hyperliquid closes connections that send nothing for 60 s. We ping often
// so that its pong arrives every few seconds even with no book subscribed:
// then 15 s of silence surely means a dead connection (seen live 2026-11-22:
// with the old 60 s, a Wi-Fi cut was noticed only after a minute).
constexpr auto kPingInterval = std::chrono::seconds(5);
constexpr auto kIdleTimeout = std::chrono::seconds(15);
constexpr int kMaxBackoffSeconds = 30;

Timestamp wall_now() {
    return std::chrono::duration_cast<Timestamp>(std::chrono::system_clock::now().time_since_epoch());
}

int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// "0x1234567890abcdef...5678" -> "0x1234...5678", for log lines.
std::string short_address(std::string_view address) {
    if (address.size() <= 12) return std::string(address);
    return std::string(address.substr(0, 6)) + "..." + std::string(address.substr(address.size() - 4));
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

const bool kDebug = std::getenv("PEEWEE_DEBUG") != nullptr;

}  // namespace

// THREADING: every WebSocket callback, timer and on_event() call runs on
// thread_ (the single network thread). Engine-thread calls (subscribe_book,
// resync) are posted to it with asio::post.
struct HyperliquidVenue::Net {
    explicit Net(asio::io_context& io) : work(asio::make_work_guard(io)), ping_timer(io), retry_timer(io) {}

    // Keeps io.run() alive even when there is nothing to do (e.g. while reconnecting).
    asio::executor_work_guard<asio::io_context::executor_type> work;
    asio::steady_timer ping_timer;
    asio::steady_timer retry_timer;
    int backoff_s = 1;                    // Network thread.
    bool open = false;                    // Network thread.
    std::vector<std::string> book_coins;  // Network thread: coins to (re)subscribe.
    bool private_wanted = false;          // Network thread: subscribe_private() was called.
    hyperliquid::LiveOids live_oids;      // Network thread: current oid of each open order.

    struct Sent {
        ClientOrderId cid;
        RequestKind kind;
    };
    std::unordered_map<RequestId, Sent> sent;

    // Blocking REST calls (query_order) run here, never on the network thread.
    asio::thread_pool rest_pool{1};

    // connect() waits for the first on_open.
    std::mutex m;
    std::condition_variable cv;
    bool up = false;
};

HyperliquidVenue::HyperliquidVenue(VenueId id, HyperliquidConfig cfg) : id_(id), cfg_(std::move(cfg)) {
    caps_.native_amend = true;        // "modify" / "batchModify" action.
    caps_.amend_keeps_priority = false;
    caps_.ws_order_entry = true;      // "post" over WebSocket.
    caps_.post_only = true;           // tif "Alo" (add liquidity only)
    caps_.book_checksum = false;
    caps_.max_book_depth = 20;        // l2Book: 20 levels per side.
}

HyperliquidVenue::~HyperliquidVenue() { disconnect(); }


void HyperliquidVenue::add_instrument(const Instrument& inst) {
    instruments_[inst.id] = inst;
    by_coin_[inst.symbol] = inst.id;
}

void HyperliquidVenue::connect() {
    if (!sink_) throw std::logic_error("HyperliquidVenue::connect: set_sink() was not called");
    if (thread_.joinable()) return;  // Already connected.

    load_assets();
    if (!cfg_.api_wallet_key.empty()) {
        signer_ = std::make_unique<hyperliquid::Signer>(cfg_.api_wallet_key, !cfg_.testnet);
        check_api_wallet();
    } else {
        PEEWEE_LOG("hyperliquid: no API wallet key, public data only (orders will be rejected)");
    }

    for (const auto& [id, inst] : instruments_) {
        coins_[inst.symbol] = &inst;
    }

    // On a second connect(), destroy the old objects before their io_context.
    ws_.reset();
    net_.reset();
    io_ = std::make_unique<asio::io_context>();
    net_ = std::make_unique<Net>(*io_);
    const std::string url = cfg_.testnet ? "wss://api.hyperliquid-testnet.xyz/ws"
                                         : "wss://api.hyperliquid.xyz/ws";

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

    // Block until the connection is open, so subscribe_book() right after
    // connect() is sent immediately.
    bool ok = false;
    {
        std::unique_lock lock(net_->m);
        ok = net_->cv.wait_for(lock, kConnectTimeout, [this] { return net_->up; });
    }
    if (!ok) {
        disconnect();
        throw std::runtime_error("HyperliquidVenue::connect: timed out");
    }
}

HyperliquidInstrumentSpec HyperliquidVenue::fetch_instrument(std::string_view coin, double ref_px) const {
    transport::HttpClient http(cfg_.testnet ? "https://api.hyperliquid-testnet.xyz" : "https://api.hyperliquid.xyz");
    const auto res = http.post("/info", R"({"type":"meta"})", {{"Content-Type", "application/json"}});
    const auto meta = hyperliquid::parse_meta(res.body);
    if (!meta) {
        throw std::runtime_error("HyperliquidVenue::fetch_instrument: unreadable /info meta (HTTP " +
                                 std::to_string(res.status) + ")");
    }
    const auto it = meta->find(std::string(coin));
    if (it == meta->end()) {
        throw std::runtime_error("HyperliquidVenue::fetch_instrument: unknown coin '" + std::string(coin) + "'");
    }
    if (ref_px <= 0.0) ref_px = fetch_mid(coin);  // No reference given: the current mid.
    HyperliquidInstrumentSpec spec;
    spec.lot_size = std::pow(10.0, -it->second.sz_decimals);
    spec.tick_size = hyperliquid::tick_for_price(ref_px, it->second.sz_decimals);
    spec.min_qty = spec.lot_size;
    return spec;
}

double HyperliquidVenue::fetch_mid(std::string_view coin) const {
    transport::HttpClient http(cfg_.testnet ? "https://api.hyperliquid-testnet.xyz" : "https://api.hyperliquid.xyz");
    const auto res = http.post("/info", R"({"type":"allMids"})", {{"Content-Type", "application/json"}});
    const auto mid = hyperliquid::parse_mid(res.body, coin);
    if (!mid) {
        throw std::runtime_error("HyperliquidVenue::fetch_mid: no mid price for '" + std::string(coin) + "' (HTTP " +
                                 std::to_string(res.status) + ")");
    }
    return *mid;
}

void HyperliquidVenue::load_assets() {
    transport::HttpClient http(cfg_.testnet ? "https://api.hyperliquid-testnet.xyz" : "https://api.hyperliquid.xyz");
    const auto res = http.post("/info", R"({"type":"meta"})", {{"Content-Type", "application/json"}});
    const auto meta = hyperliquid::parse_meta(res.body);
    if (!meta) {
        throw std::runtime_error("HyperliquidVenue::connect: unreadable /info meta (HTTP " + std::to_string(res.status) +
                                 ")");
    }
    for (const auto& [id, inst] : instruments_) {
        const auto it = meta->find(inst.symbol);
        if (it == meta->end()) {
            throw std::runtime_error("HyperliquidVenue::connect: unknown coin '" + inst.symbol + "'");
        }
        // The lot is fixed by szDecimals: 5 -> 0.00001. A wrong lot would
        // silently round every quantity, so refuse to start.
        const double lot = std::pow(10.0, -it->second.sz_decimals);
        if (std::abs(inst.lot_size - lot) > 1e-9 * lot) {
            char text[200];
            std::snprintf(text, sizeof(text), "%s has lot %g on Hyperliquid (szDecimals %d), but %g was configured",
                          inst.symbol.c_str(), lot, it->second.sz_decimals, inst.lot_size);
            throw std::runtime_error(std::string("HyperliquidVenue::connect: ") + text);
        }
        assets_[id] = Asset{it->second.index, it->second.sz_decimals};
    }
}

void HyperliquidVenue::check_api_wallet() {
    // Ask Hyperliquid whose API wallet this key is. Catches a wrong key, or
    // an API wallet that was not approved, before any order is signed.
    transport::HttpClient http(cfg_.testnet ? "https://api.hyperliquid-testnet.xyz" : "https://api.hyperliquid.xyz");
    const auto res = http.post("/info", R"({"type":"userRole","user":")" + signer_->address() + R"("})",
                               {{"Content-Type", "application/json"}});
    const auto owner = hyperliquid::parse_agent_owner(res.body);
    if (!owner) {
        throw std::runtime_error("HyperliquidVenue::connect: " + signer_->address() +
                                 " is not an approved API wallet (answer: " + res.body + ")");
    }
    if (cfg_.account_address.empty()) {
        throw std::runtime_error("HyperliquidVenue::connect: account_address is empty");
    }
    if (lowercase(*owner) != lowercase(cfg_.account_address)) {
        throw std::runtime_error("HyperliquidVenue::connect: API wallet " + signer_->address() + " belongs to " +
                                 *owner + ", not to " + cfg_.account_address);
    }
    // Shortened (0x1234...5678): enough to recognize them, safer in shared logs.
    PEEWEE_LOG("hyperliquid: API wallet %s, account %s", short_address(signer_->address()).c_str(),
               short_address(*owner).c_str());
}

void HyperliquidVenue::disconnect() {
    if (!thread_.joinable()) return;

    // Wait for a query in progress: it posts its answer to the network thread.
    net_->rest_pool.join();

    // Stop timers and close the socket ON the network thread, then wait.
    std::promise<void> closed;
    asio::post(*io_, [this, &closed] {
        net_->retry_timer.cancel();
        net_->ping_timer.cancel();
        ws_->close();
        closed.set_value();
    });
    closed.get_future().wait();

    net_->work.reset();
    io_->stop();
    thread_.join();
}

// --- Network thread ---

void HyperliquidVenue::on_open() {
    net_->backoff_s = 1;
    net_->open = true;
    // After a reconnect: ask again for every book. Hyperliquid answers with full books.
    for (const auto& coin : net_->book_coins) {
        ws_->send(hyperliquid::make_subscribe_l2book(coin));
    }
    sink_->on_event(VenueStatus{id_, StreamKind::Public, true, wall_now()});
    if (signer_) {
        // One connection carries everything: orders can go out as soon as it is open.
        sink_->on_event(VenueStatus{id_, StreamKind::Trading, true, wall_now()});
    }
    if (net_->private_wanted) send_private_subscriptions();

    {
        std::lock_guard lock(net_->m);
        net_->up = true;
    }
    net_->cv.notify_all();
}

void HyperliquidVenue::on_message(std::string_view msg) {
    // PEEWEE_DEBUG=1 prints every message received, except books (too many).
    // Pongs too: one every 5 s would drown the useful lines.
    if (kDebug && !msg.starts_with(R"({"channel":"l2Book")") && msg != R"({"channel":"pong"})") {
        PEEWEE_LOG("hyperliquid <- %.*s", static_cast<int>(msg.size()), msg.data());
    }

    if (auto delta = hyperliquid::parse_l2book(msg, coins_)) {
        sink_->on_event(std::move(*delta));
        return;
    }
    // Answers to our ping and subscribe: nothing to do.
    if (msg.find(R"("channel":"pong")") != std::string_view::npos ||
        msg.find(R"("channel":"subscriptionResponse")") != std::string_view::npos) {
        return;
    }
    if (msg.starts_with(R"({"channel":"orderUpdates")")) {
        for (auto& event : hyperliquid::parse_order_updates(msg, coins_, net_->live_oids)) {
            sink_->on_event(std::move(event));
        }
        return;
    }
    // Answer to one of our orders / modifies / cancels. Successes need nothing
    // ("orderUpdates" brings the acks); an error is the only news that the
    // request failed, and it only carries our request number.
    if (msg.starts_with(R"({"channel":"post")")) {
        if (auto err = hyperliquid::parse_post_response(msg)) {
            const auto it = net_->sent.find(err->id);
            if (it == net_->sent.end()) {
                PEEWEE_LOG("hyperliquid: error for unknown request %llu: %s",
                           static_cast<unsigned long long>(err->id), err->text.c_str());
                return;
            }
            const auto [cid, kind] = it->second;
            net_->sent.erase(it);
            if (kind == RequestKind::New && hyperliquid::is_ioc_without_match(err->text)) {
                // A market / IOC order that found nothing within its price:
                // canceled, not rejected (like Bybit and the simulator).
                sink_->on_event(OrderEvent{OrderClosed{cid, err->id, CloseReason::Canceled, wall_now()}});
                return;
            }
            OrderReject r;
            r.cid = cid;
            r.req = err->id;
            r.request = kind;
            r.reason = hyperliquid::reject_reason_from_text(err->text);
            r.text = err->text;
            r.ts = wall_now();
            sink_->on_event(OrderEvent{r});
        }
        // TODO: successes stay in net_->sent (a few bytes each); the answer
        // would need its id parsed too to erase them.
        return;
    }
    if (msg.starts_with(R"({"channel":"userFills")")) {
        for (auto& event : hyperliquid::parse_user_fills(msg, coins_)) {
            sink_->on_event(std::move(event));
        }
        return;
    }
    // Errors ({"channel":"error",...}) and anything we do not understand.
    if (kDebug) return;  // Already printed in full above.
    PEEWEE_LOG("hyperliquid: unexpected message: %.*s", static_cast<int>(std::min<size_t>(msg.size(), 200)),
               msg.data());
}

void HyperliquidVenue::on_close(std::string_view reason) {
    PEEWEE_LOG("hyperliquid: connection lost (%.*s), reconnecting in %d s", static_cast<int>(reason.size()),
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

void HyperliquidVenue::send_private_subscriptions() {
    ws_->send(hyperliquid::make_subscribe_user("orderUpdates", cfg_.account_address));
    ws_->send(hyperliquid::make_subscribe_user("userFills", cfg_.account_address));
    sink_->on_event(VenueStatus{id_, StreamKind::Private, true, wall_now()});
}

void HyperliquidVenue::schedule_ping() {
    net_->ping_timer.expires_after(kPingInterval);
    net_->ping_timer.async_wait([this](const boost::system::error_code& ec) {
        if (ec) return;  // Cancelled by disconnect().
        if (ws_->is_open()) ws_->send(R"({"method":"ping"})");
        schedule_ping();
    });
}

// --- Engine thread ---

void HyperliquidVenue::subscribe_book(InstrumentId inst, int /*depth*/) {
    // depth is ignored: l2Book always has up to 20 levels per side.
    const auto it = instruments_.find(inst);
    if (it == instruments_.end()) {
        PEEWEE_LOG("hyperliquid: subscribe_book for unknown instrument %u", inst);
        return;
    }
    if (!thread_.joinable()) {
        PEEWEE_LOG("hyperliquid: subscribe_book before connect()");
        return;
    }
    asio::post(*io_, [this, coin = it->second.symbol] {
        net_->book_coins.push_back(coin);
        if (net_->open) ws_->send(hyperliquid::make_subscribe_l2book(coin));
    });
}

void HyperliquidVenue::subscribe_private() {
    if (cfg_.account_address.empty()) return;  // Public data only (e.g. hl_book).
    if (!thread_.joinable()) {
        PEEWEE_LOG("hyperliquid: subscribe_private before connect()");
        return;
    }
    asio::post(*io_, [this] {
        if (net_->private_wanted) return;
        net_->private_wanted = true;
        if (net_->open) send_private_subscriptions();
    });
}

void HyperliquidVenue::reject_later(ClientOrderId cid, RequestId req, RequestKind kind, RejectReason reason,
                                    std::string text) {
    PEEWEE_LOG("hyperliquid: %s request %llu not sent: %s", to_string(kind), static_cast<unsigned long long>(req),
               text.c_str());
    if (!thread_.joinable()) return;
    // on_event() must always be called from the network thread.
    asio::post(*io_, [this, cid, req, kind, reason, text = std::move(text)] {
        OrderReject r;
        r.cid = cid;
        r.req = req;
        r.request = kind;
        r.reason = reason;
        r.text = text;
        r.ts = wall_now();
        sink_->on_event(OrderEvent{r});
    });
}

void HyperliquidVenue::send_signed(ClientOrderId cid, RequestId req, RequestKind kind, std::string_view action_json,
                                   std::string_view action_msgpack) {
    // Nonces must be unique and close to the current time: use milliseconds,
    // +1 if two requests fall in the same millisecond.
    const int64_t nonce = std::max(last_nonce_ + 1, wall_ms());
    last_nonce_ = nonce;

    const auto sig = signer_->sign_l1_action(action_msgpack, nonce);
    std::string message = hyperliquid::make_post(req, action_json, nonce, sig.r, sig.s, sig.v);
    if (kDebug) PEEWEE_LOG("hyperliquid -> %s", message.c_str());  // No secrets in it: only the signature.

    asio::post(*io_, [this, cid, req, kind, message = std::move(message)] {
        if (!net_->open) {
            OrderReject r;
            r.cid = cid;
            r.req = req;
            r.request = kind;
            r.reason = RejectReason::VenueUnavailable;
            r.text = "not connected";
            r.ts = wall_now();
            sink_->on_event(OrderEvent{r});
            return;
        }
        net_->sent[req] = {cid, kind};
        ws_->send(message);
    });
}

namespace {

// The order's "t" field, or nullopt if Hyperliquid cannot do this order
// (FOK, or a trigger price it would reject).
std::optional<hyperliquid::OrderKind> order_kind(const NewOrder& o, const Instrument& inst, int sz_decimals) {
    std::string trigger_px;
    if (is_trigger(o.type)) {
        trigger_px = hyperliquid::format_wire(o.trigger_px.ticks, inst.tick_size);
        if (o.trigger_px.ticks <= 0 || !hyperliquid::is_valid_price(trigger_px, sz_decimals)) {
            return std::nullopt;
        }
    }
    return hyperliquid::order_kind(o.type, o.tif, std::move(trigger_px));
}

}  // namespace

void HyperliquidVenue::send_new(const NewOrder& o) {
    const auto inst_it = instruments_.find(o.inst);
    const auto asset_it = assets_.find(o.inst);
    if (!signer_ || !thread_.joinable() || inst_it == instruments_.end() || asset_it == assets_.end()) {
        reject_later(o.cid, o.req, RequestKind::New, RejectReason::VenueUnavailable,
                     signer_ ? "unknown instrument or not connected" : "no API wallet key");
        return;
    }
    const Instrument& inst = inst_it->second;
    const Asset& asset = asset_it->second;

    const std::string px = hyperliquid::format_wire(o.px.ticks, inst.tick_size);
    if (!hyperliquid::is_valid_price(px, asset.sz_decimals)) {
        reject_later(o.cid, o.req, RequestKind::New, RejectReason::InvalidPrice,
                     "price " + px + " has too many significant digits or decimals for Hyperliquid");
        return;
    }
    const std::string sz = hyperliquid::format_wire(o.qty.lots, inst.lot_size);

    const auto kind = order_kind(o, inst, asset.sz_decimals);
    if (!kind) {
        reject_later(o.cid, o.req, RequestKind::New, RejectReason::Unknown,
                     std::string(to_string(o.type)) + " with this tif / trigger price is not possible on Hyperliquid");
        return;
    }
    const auto action = hyperliquid::make_order_action(asset.index, o.side == Side::Buy, px, sz, o.reduce_only, *kind,
                                                       hyperliquid::make_cloid(o.cid));
    orders_[o.cid] = o;
    send_signed(o.cid, o.req, RequestKind::New, action.json, action.msgpack);
}

void HyperliquidVenue::send_amend(const AmendOrder& o) {
    // "modify" replaces the whole order, so it needs side, size and tif too.
    const auto order_it = orders_.find(o.cid);
    if (!signer_ || !thread_.joinable() || order_it == orders_.end()) {
        reject_later(o.cid, o.req, RequestKind::Amend, RejectReason::OrderNotFound, "order unknown to this venue");
        return;
    }
    NewOrder& order = order_it->second;
    const Instrument& inst = instruments_.at(order.inst);
    const Asset& asset = assets_.at(order.inst);

    const std::string px = hyperliquid::format_wire(o.px.ticks, inst.tick_size);
    if (!hyperliquid::is_valid_price(px, asset.sz_decimals)) {
        reject_later(o.cid, o.req, RequestKind::Amend, RejectReason::InvalidPrice,
                     "price " + px + " has too many significant digits or decimals for Hyperliquid");
        return;
    }
    const Qty qty = o.qty.value_or(order.qty);
    const std::string sz = hyperliquid::format_wire(qty.lots, inst.lot_size);

    const auto kind = order_kind(order, inst, asset.sz_decimals);  // Was valid at send_new: cannot fail.
    const auto action = hyperliquid::make_modify_action(asset.index, order.side == Side::Buy, px, sz, order.reduce_only,
                                                        *kind, hyperliquid::make_cloid(o.cid));
    // TODO: if this modify is rejected, these are not the terms on the exchange
    // any more. Harmless for price (every amend sends a new one), wrong for qty.
    order.px = o.px;
    order.qty = qty;
    send_signed(o.cid, o.req, RequestKind::Amend, action.json, action.msgpack);
}

void HyperliquidVenue::send_cancel(const CancelOrder& o) {
    const auto asset_it = assets_.find(o.inst);
    if (!signer_ || !thread_.joinable() || asset_it == assets_.end()) {
        reject_later(o.cid, o.req, RequestKind::Cancel, RejectReason::VenueUnavailable,
                     signer_ ? "unknown instrument or not connected" : "no API wallet key");
        return;
    }
    const auto action = hyperliquid::make_cancel_action(asset_it->second.index, hyperliquid::make_cloid(o.cid));
    // TODO: erase orders_[cid] once OrderClosed is parsed from orderUpdates.
    send_signed(o.cid, o.req, RequestKind::Cancel, action.json, action.msgpack);
}

void HyperliquidVenue::query_order(const QueryOrder& q) {
    // POST /info {"type":"orderStatus",...} by cloid. It also finds closed
    // orders, and after a modify it answers for the current copy.
    if (!thread_.joinable() || cfg_.account_address.empty()) return;
    std::string body = hyperliquid::make_order_status_request(cfg_.account_address, hyperliquid::make_cloid(q.cid));
    asio::post(net_->rest_pool, [this, cid = q.cid, body = std::move(body)] {
        try {
            transport::HttpClient http(cfg_.testnet ? "https://api.hyperliquid-testnet.xyz"
                                                    : "https://api.hyperliquid.xyz");
            const auto res = http.post("/info", body, {{"Content-Type", "application/json"}});
            auto status = hyperliquid::parse_order_status(res.body, coins_, cid);
            if (!status) {
                // No event: the OMS asks again after its timeout.
                PEEWEE_LOG("hyperliquid: order query for cid=%llu failed (HTTP %d): %.200s",
                           static_cast<unsigned long long>(cid), res.status, res.body.c_str());
                return;
            }
            if (!status->ts.count()) status->ts = wall_now();
            if (kDebug) {
                PEEWEE_LOG("hyperliquid: order query cid=%llu: %s, %s", static_cast<unsigned long long>(cid),
                           status->found ? "found" : "NOT found", status->open ? "open" : "closed");
            }
            // on_event() only from the network thread.
            asio::post(*io_, [this, s = *status] { sink_->on_event(OrderEvent{s}); });
        } catch (const std::exception& e) {
            PEEWEE_LOG("hyperliquid: order query for cid=%llu failed: %s", static_cast<unsigned long long>(cid),
                       e.what());
        }
    });
}

void HyperliquidVenue::resync() {
    // The engine lost events. Every l2Book message is a full book, so the
    // next one repairs the books by itself: just say the stream is back.
    // Lost order events: Private-up makes the OMS query every open order.
    if (!thread_.joinable()) return;
    asio::post(*io_, [this] {
        if (!net_->open) return;
        sink_->on_event(VenueStatus{id_, StreamKind::Public, true, wall_now()});
        if (net_->private_wanted) sink_->on_event(VenueStatus{id_, StreamKind::Private, true, wall_now()});
    });
}

bool HyperliquidVenue::can_send(RequestKind /*kind*/) const {
    // TODO: Hyperliquid limits depend on the address and traded volume.
    return true;
}

}  // namespace peewee
