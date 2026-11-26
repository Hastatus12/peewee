#pragma once
// Simple interface: one object per exchange, one call per order.
//
//   auto hl = peewee::Exchange::hyperliquid({"ETH", "BTC"});  // keys from HL_ACCOUNT, HL_API_KEY
//   auto order = hl.chase("ETH", peewee::Side::Buy, 0.01, {.max_move_pct = 0.5});
//   order.wait();
//   std::printf("%s, filled %g at %g\n", order.state_text(), order.filled(), order.avg_price());
//
// Quantities and prices are plain numbers in the exchange's units; tick and
// lot sizes are read from the exchange when the Exchange is created.
//
// The engine runs on a background thread owned by the Exchange, so every call
// returns at once and every method is thread-safe. Destroying the Exchange
// cancels the orders still working (waiting up to 10 s for the exchange to
// confirm) and disconnects.
//
// For your own algorithms use the lower level directly: Engine, Algo, the
// venue classes (see the README).

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "peewee/core/types.hpp"

namespace peewee {

class Engine;
class IVenue;

struct ChaseOptions {
    double max_move_pct = 1.0;  // Used when limit is 0: buy at most this % above the ask
                                // seen at start, sell at least this % below the bid.
    double limit = 0.0;         // Hard limit price instead of max_move_pct.
    std::chrono::milliseconds reprice_every{1000};  // Minimum time between amends.
    int max_reprices = 100;
    std::chrono::seconds max_duration{0};           // 0 = no limit; then the order is canceled.
    int offset_ticks = 0;                           // Ticks in front of the best price.
};

struct LimitOptions {
    Tif tif = Tif::GTC;
    bool reduce_only = false;
};

struct MarketOptions {
    double max_move_pct = 1.0;  // Worst price: this % above the ask (buy) / below the bid (sell).
    bool reduce_only = false;
};

class OrderHandle {
public:
    enum class State : uint8_t { Working, Filled, Canceled, Rejected, Expired };

    State state() const;
    const char* state_text() const;  // "working", "filled", ...
    bool done() const;               // Anything but Working.

    double filled() const;     // Quantity filled so far.
    double avg_price() const;  // Average price of the fills received (0 if none).
    std::string error() const; // The exchange's reason, if Rejected.

    void wait() const;                                  // Until done.
    bool wait_for(std::chrono::milliseconds max) const; // False on timeout.
    void cancel();                                      // Asynchronous: see state().

    struct Shared;  // Internal.

private:
    friend class Exchange;
    std::shared_ptr<Shared> s_;
};

class Exchange {
public:
    // Keys from the environment: HL_ACCOUNT (main account address) and
    // HL_API_KEY (API wallet key). Throws if missing or refused.
    static Exchange hyperliquid(const std::vector<std::string>& coins, bool testnet = false);
    // Keys from BYBIT_API_KEY and BYBIT_API_SECRET. Linear contracts.
    static Exchange bybit(const std::vector<std::string>& symbols, bool testnet = false);
    // Keys from LIGHTER_ACCOUNT_INDEX, LIGHTER_API_KEY_INDEX and LIGHTER_API_KEY
    // (the API key's private key). Perpetuals: "BTC", "ETH", ...
    static Exchange lighter(const std::vector<std::string>& symbols, bool testnet = false);

    // Advanced: an engine already set up (venue added, instruments added,
    // connected). The Exchange takes it over and runs it.
    Exchange(std::unique_ptr<Engine> engine, IVenue& venue);

    Exchange(Exchange&&) noexcept;
    Exchange& operator=(Exchange&&) noexcept;
    ~Exchange();

    // All three throw std::invalid_argument for a symbol not given at creation.
    OrderHandle chase(std::string_view symbol, Side side, double qty, ChaseOptions opt = {});
    OrderHandle limit(std::string_view symbol, Side side, double qty, double price, LimitOptions opt = {});
    OrderHandle market(std::string_view symbol, Side side, double qty, MarketOptions opt = {});

    void cancel_all();

    // Advanced: run `fn` on the engine thread (e.g. to drive a simulator).
    void post(std::function<void(Engine&)> fn);

    struct Impl;  // Internal.

private:
    std::shared_ptr<Impl> impl_;  // Shared with the handles, so cancel() is safe after the Exchange is gone.
};

}  // namespace peewee
