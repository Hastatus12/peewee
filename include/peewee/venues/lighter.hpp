#pragma once
// Lighter adapter (perpetuals): translates between Lighter and the library.

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include "peewee/venue/venue.hpp"

namespace peewee::transport {
class WsClient;
}  // namespace peewee::transport

namespace boost::asio {
class io_context;
}

namespace peewee::lighter {
class Signer;
}  // namespace peewee::lighter

namespace peewee {

// Without api_private_key only public data works (order books): requests are
// rejected with VenueUnavailable.
struct LighterConfig {
    // The account (an integer, shown on app.lighter.xyz) and the index of the
    // API key used to sign (0-254, registered on that account).
    int64_t account_index = -1;
    int api_key_index = -1;
    // The API key's private key, 40 bytes in hex. Never put it in code: read
    // it from the environment. connect() checks with Lighter that its public
    // key is the one registered at (account_index, api_key_index).
    std::string api_private_key;
    bool testnet = true;
};

// Trading rules of one market, as Lighter publishes them.
struct LighterInstrumentSpec {
    double tick_size = 0.0;  // 10^-price_decimals
    double lot_size = 0.0;   // 10^-size_decimals
    double min_qty = 0.0;    // min_base_amount (there is also a minimum order value, about 10 USD).
};

class LighterVenue final : public IVenue {
public:
    LighterVenue(VenueId id, LighterConfig cfg);
    ~LighterVenue() override;

    // Ask Lighter for a perpetual market's tick/lot sizes (public REST, no key).
    // symbol: "BTC", "ETH", ... Blocking: call before connect(). Throws if
    // the market does not exist or Lighter cannot be reached.
    LighterInstrumentSpec fetch_instrument(std::string_view symbol) const;

    VenueId id() const override { return id_; }
    std::string_view name() const override { return "lighter"; }
    const VenueCapabilities& caps() const override { return caps_; }
    void set_sink(IEventSink* sink) override { sink_ = sink; }
    void add_instrument(const Instrument& inst) override;
    // Also checks every instrument's tick/lot sizes against Lighter and throws
    // on a mismatch, checks the API key and reads its next nonce.
    void connect() override;
    void disconnect() override;
    void subscribe_book(InstrumentId inst, int depth) override;
    void subscribe_private() override;
    void send_new(const NewOrder& o) override;
    void send_amend(const AmendOrder& o) override;
    void send_cancel(const CancelOrder& o) override;
    void query_order(const QueryOrder& q) override;
    void resync() override;
    bool can_send(RequestKind kind) const override;

private:
    struct Net;  // Timers and network-thread state, defined in lighter_venue.cpp.

    // Network-thread handlers.
    void on_open();
    void on_message(std::string_view msg);
    void on_close(std::string_view reason);
    void schedule_ping();
    // Every subscribed market, with a fresh snapshot; Public goes up again
    // once they have all arrived (public_up_when_ready).
    void subscribe_books(bool resubscribe);
    void public_up_when_ready();
    void send_private_subscriptions();
    void forward(OrderEvent&& e);  // Maps Lighter's client_order_index back to our cid.
    void emit_reject(ClientOrderId cid, RequestId req, RequestKind kind, RejectReason reason, std::string text);
    // Re-read the API key's nonce from Lighter (on the REST thread), then
    // re-sign the transactions refused for a wrong nonce.
    void refresh_nonce();
    void on_nonce(std::optional<int64_t> nonce);
    void prune_sent();

    // Startup (blocking REST calls, before the network thread starts).
    const std::string& market_details() const;  // GET orderBookDetails, once.
    void load_markets();
    void check_api_key();

    // Signs a transaction with this nonce: the whole "jsonapi/sendtx" message.
    // Kept to re-sign it if Lighter refuses the nonce. Thread-safe.
    using TxSigner = std::function<std::string(int64_t nonce)>;
    // Engine thread: sign and hand a tx to the network thread.
    void send_tx(ClientOrderId cid, RequestId req, RequestKind kind, TxSigner sign);
    // Engine thread: report a request that never left (emitted on the network thread).
    void reject_later(ClientOrderId cid, RequestId req, RequestKind kind, RejectReason reason, std::string text);
    int64_t next_nonce();
    // Engine thread: remember a new order (evicting the oldest beyond a limit).
    void remember_order(const NewOrder& o, int64_t coi);

    VenueId id_;
    LighterConfig cfg_;
    VenueCapabilities caps_;
    IEventSink* sink_ = nullptr;

    // Filled by add_instrument() / connect(), read-only afterwards.
    mutable std::string market_details_;  // Cached orderBookDetails answer.
    std::unordered_map<InstrumentId, Instrument> instruments_;
    std::unordered_map<InstrumentId, int64_t> market_ids_;  // Lighter's market_id (the codec's map is in Net).

    // Order matters: members are destroyed bottom-up, and everything below
    // uses io_, so io_ must be destroyed last.
    std::unique_ptr<boost::asio::io_context> io_;
    std::thread thread_;  // Runs io_->run().
    std::unique_ptr<Net> net_;

    std::unique_ptr<lighter::Signer> signer_;  // Read-only after connect(): sign() is thread-safe.
    std::atomic<int64_t> nonce_{0};            // Next nonce; reset by refresh_nonce().

    // Our ClientOrderIds are larger than Lighter's 2^48 limit (the engine
    // seeds them with the clock in ms * 1000): orders are sent with cid mod
    // 2^48, and events come back with that. Written on the engine thread,
    // updated on the network thread, read on the REST thread: under orders_m_.
    struct OrderInfo {
        ClientOrderId cid = 0;
        InstrumentId inst = 0;
        uint32_t trigger_px = 0;  // Resent with every modify (0: not a trigger order).
        int64_t qty = 0;          // Size in lots as Lighter last confirmed it (sent size until then).
        int64_t sent_ms = 0;      // When the create was sent (wall clock).
        bool live = false;        // Seen open on Lighter.
    };
    std::mutex orders_m_;
    std::unordered_map<int64_t, OrderInfo> orders_;  // client_order_index -> order
    std::deque<int64_t> order_fifo_;                 // Engine thread: creation order, for eviction.

    std::unique_ptr<transport::WsClient> ws_;  // Market data, private events and order entry.
};

}  // namespace peewee
