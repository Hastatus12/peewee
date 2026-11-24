#pragma once
// Basic types shared by the entire library. This file has no dependencies.

#include <chrono>
#include <compare>
#include <cstdint>

namespace peewee {

// ---------------------------------------------------------------------------
// Fixed-point prices and quantities: integers, never doubles.
//
// Prices are expressed as a number of ticks (the instrument's minimum price
// increment). Example: BTCUSDT on Bybit has tick size 0.1, so 65000.3 -> 650003.
// Integer comparisons are exact (65000.3 == 65000.3 always), and
// "one tick higher" is simply +1.
// ---------------------------------------------------------------------------
struct Price {
    int64_t ticks = 0;

    constexpr auto operator<=>(const Price&) const = default;

    constexpr Price operator+(int64_t n) const { return {ticks + n}; }
    constexpr Price operator-(int64_t n) const { return {ticks - n}; }
    // Distance between two prices, in ticks.
    constexpr int64_t operator-(Price o) const { return ticks - o.ticks; }
};

// Quantity expressed as a number of lots (the minimum quantity increment).
struct Qty {
    int64_t lots = 0;

    constexpr auto operator<=>(const Qty&) const = default;

    constexpr Qty operator+(Qty o) const { return {lots + o.lots}; }
    constexpr Qty operator-(Qty o) const { return {lots - o.lots}; }
    constexpr Qty& operator+=(Qty o) { lots += o.lots; return *this; }
    constexpr Qty& operator-=(Qty o) { lots -= o.lots; return *this; }
    constexpr bool is_zero() const { return lots == 0; }
};

// Internal identifiers: small, dense numbers usable as array indices.
using InstrumentId  = uint32_t;
using VenueId       = uint16_t;
using AlgoId        = uint32_t;
using ClientOrderId = uint64_t;   // Generated locally; the exchange returns it in events.
using RequestId     = uint64_t;   // Identifies one New/Amend/Cancel request; echoed in its response.

inline constexpr ClientOrderId kNoOrder = 0;
inline constexpr RequestId kNoRequest = 0;  // Unsolicited event (not a response to a request).

enum class Side : uint8_t { Buy, Sell };

constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }
constexpr const char* to_string(Side s) { return s == Side::Buy ? "Buy" : "Sell"; }

// Time in force: how long an order remains valid.
enum class Tif : uint8_t {
    GTC,       // Remains in the book until filled or canceled.
    IOC,       // Fill immediately where possible, cancel the remainder.
    PostOnly,  // Enter the book only as maker; reject if it would execute immediately.
    FOK,       // Fill the whole quantity immediately, or nothing. Bybit only.
};

// What kind of order. px is always given, and means:
//  - Limit / StopLimit / TakeProfitLimit: the limit price;
//  - Market / StopMarket / TakeProfitMarket: the WORST price accepted. The
//    order executes immediately up to that price and the rest is canceled:
//    it protects against an empty book. (Hyperliquid has no other kind of
//    market order; on Bybit it is sent as a Limit IOC for the same behavior.)
//
// Trigger orders (Stop*, TakeProfit*) wait until the MARK price reaches
// trigger_px, then become a Market / Limit order:
//  - Stop:       a buy triggers when the price RISES to trigger_px,
//                a sell when it FALLS to it (limits a loss, or enters a breakout);
//  - TakeProfit: a buy triggers when the price FALLS to trigger_px,
//                a sell when it RISES to it.
// Before the trigger they are not in the book (and cannot be filled).
//
// WARNING, Hyperliquid StopMarket / TakeProfitMarket: when they trigger,
// Hyperliquid replaces px with its own limit, trigger_px +-10% (seen live
// 2026-11-22: trigger 61054, our px 61347, used 67159). px does NOT protect
// these two there. Use StopLimit / TakeProfitLimit for a hard limit.
enum class OrderType : uint8_t {
    Limit,
    Market,
    StopMarket,
    StopLimit,
    TakeProfitMarket,
    TakeProfitLimit,
};

constexpr bool is_trigger(OrderType t) { return t != OrderType::Limit && t != OrderType::Market; }
// Executes immediately (after the trigger, for trigger orders) and never rests in the book.
constexpr bool is_market(OrderType t) {
    return t == OrderType::Market || t == OrderType::StopMarket || t == OrderType::TakeProfitMarket;
}
constexpr bool is_stop(OrderType t) { return t == OrderType::StopMarket || t == OrderType::StopLimit; }

constexpr const char* to_string(OrderType t) {
    switch (t) {
        case OrderType::Limit:            return "Limit";
        case OrderType::Market:           return "Market";
        case OrderType::StopMarket:       return "StopMarket";
        case OrderType::StopLimit:        return "StopLimit";
        case OrderType::TakeProfitMarket: return "TakeProfitMarket";
        case OrderType::TakeProfitLimit:  return "TakeProfitLimit";
    }
    return "?";
}

// Trigger orders: true if the price `rises` to the trigger (see OrderType).
// buy: true for a buy order.
constexpr bool triggers_on_rise(OrderType t, bool buy) { return is_stop(t) == buy; }

// Types of requests sent to the exchange.
enum class RequestKind : uint8_t { New, Amend, Cancel };

constexpr const char* to_string(RequestKind k) {
    switch (k) {
        case RequestKind::New:    return "New";
        case RequestKind::Amend:  return "Amend";
        case RequestKind::Cancel: return "Cancel";
    }
    return "?";
}

// Time in nanoseconds. Which clock a Timestamp comes from depends on where it
// was produced: IClock::now() is monotonic (use it for intervals and deadlines),
// while exchange timestamps in events (`ts`) are wall-clock time from the venue.
// Never subtract one kind from the other.
using Duration  = std::chrono::nanoseconds;
using Timestamp = std::chrono::nanoseconds;

}  // namespace peewee
