#pragma once
// Hyperliquid adapter: translates between Hyperliquid and the library.

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <thread>

#include "peewee/venue/venue.hpp"

namespace peewee::transport {
class WsClient;
class HttpClient;
}  // namespace peewee::transport

namespace boost::asio {
class io_context;
}

namespace peewee::hyperliquid {
class Signer;
}  // namespace peewee::hyperliquid

namespace peewee {

struct HyperliquidConfig {
    // Main account address (0x...): the one holding the funds. Needed for
    // private events (orderUpdates, userFills).
    std::string account_address;
    // Private key of the API wallet (agent) used for signing. Never put it in
    // code: read it from the environment. Empty = public data only.
    // connect() checks with Hyperliquid that this key is an API wallet of
    // account_address, and refuses to start otherwise.
    std::string api_wallet_key;
    bool testnet = true;
    std::string vault_address;        // Optional: trade on behalf of a vault.
    // Dead man's switch, in seconds (0 = off, otherwise at least 10). The
    // venue keeps asking Hyperliquid to cancel ALL the account's open orders
    // this long from now, renewing it every third of that time. If the
    // program dies or loses the network for longer, Hyperliquid cancels the
    // orders by itself. disconnect() removes it. Note: it also cancels orders
    // placed by hand on the website, and Hyperliquid allows 10 triggers a day.
    int dead_man_switch_s = 0;
};

// Trading rules of one coin, as Hyperliquid's rules give them.
struct HyperliquidInstrumentSpec {
    double tick_size = 0.0;  // 10^-(6 - szDecimals): the most decimals a price may have.
    double lot_size = 0.0;   // 10^-szDecimals.
    double min_qty = 0.0;    // One lot (the real minimum is an order VALUE of 10 USD).
    int price_sig_figs = 5;  // And at most 5 significant digits: see Instrument::price_sig_figs.
};

class HyperliquidVenue final : public IVenue {
public:
    HyperliquidVenue(VenueId id, HyperliquidConfig cfg);
    ~HyperliquidVenue() override;

    // Ask Hyperliquid for a coin's lot (public REST, no key) and compute the
    // its price rules. Valid at any price: pass price_sig_figs on to
    // Engine::add_instrument. Blocking: call before connect(). Throws if the
    // coin does not exist or Hyperliquid cannot be reached.
    HyperliquidInstrumentSpec fetch_instrument(std::string_view coin) const;
    // The current mid price of a coin (public REST). Blocking. Throws on error.
    double fetch_mid(std::string_view coin) const;

    VenueId id() const override { return id_; }
    std::string_view name() const override { return "hyperliquid"; }
    const VenueCapabilities& caps() const override { return caps_; }
    void set_sink(IEventSink* sink) override { sink_ = sink; }
    void add_instrument(const Instrument& inst) override;
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
    struct Net;  // Timers and network-thread state, defined in hl_venue.cpp.

    // Network-thread handlers.
    void on_open();
    void on_message(std::string_view msg);
    void on_close(std::string_view reason);
    void schedule_ping();
    void send_private_subscriptions();
    // Dead man's switch (HyperliquidConfig::dead_man_switch_s).
    void renew_dead_man_switch();
    void schedule_dead_man_switch();

    // Any thread: sign an action and wrap it in a "post" message. Nonces and
    // signing are shared by the engine thread (orders) and the network
    // thread (dead man's switch), hence the mutex.
    std::string signed_post(uint64_t request_id, std::string_view action_json, std::string_view action_msgpack);

    // Startup (blocking REST calls, before the network thread starts).
    void load_assets();
    void check_api_wallet();

    // Engine thread: sign an action and hand it to the network thread.
    void send_signed(ClientOrderId cid, RequestId req, RequestKind kind, std::string_view action_json,
                     std::string_view action_msgpack);
    // Engine thread: report a request that never left (emitted on the network thread).
    void reject_later(ClientOrderId cid, RequestId req, RequestKind kind, RejectReason reason, std::string text);

    struct Asset {
        uint32_t index = 0;    // Field "a" of orders.
        int sz_decimals = 0;
    };

    VenueId id_;
    HyperliquidConfig cfg_;
    VenueCapabilities caps_;
    IEventSink* sink_ = nullptr;

    // Order matters: members are destroyed bottom-up, and everything below
    // uses io_, so io_ must be destroyed last.
    std::unique_ptr<boost::asio::io_context> io_;  // The network "engine".
    std::thread thread_;                           // Runs io_->run().
    std::unique_ptr<Net> net_;
    std::unordered_map<InstrumentId, Instrument> instruments_;
    std::unordered_map<std::string, InstrumentId> by_coin_;
    std::unordered_map<std::string, const Instrument*> coins_;  // "BTC" -> Instrument, for parse_l2book.
    std::unordered_map<InstrumentId, Asset> assets_;            // Filled by connect(), read-only afterwards.

    // Engine thread only (send_*), except signing (see signed_post).
    std::unique_ptr<hyperliquid::Signer> signer_;         // Set in connect(), used under sign_m_.
    std::unordered_map<ClientOrderId, NewOrder> orders_;  // Last known terms, to build "modify".
    std::mutex sign_m_;
    int64_t last_nonce_ = 0;                              // Guarded by sign_m_.

    // One WS connection for market data, private events, and order entry ("post").
    std::unique_ptr<transport::WsClient> ws_;
    std::unique_ptr<transport::HttpClient> rest_;  // /info for metadata and reconciliation.
};

}  // namespace peewee
