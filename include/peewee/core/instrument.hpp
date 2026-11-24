#pragma once
// Description of a tradable instrument (e.g. BTCUSDT perpetual on Bybit).

#include <cmath>
#include <string>

#include "peewee/core/types.hpp"

namespace peewee {

struct Instrument {
    InstrumentId id = 0;
    VenueId venue = 0;
    std::string symbol;        // Exchange symbol: "BTCUSDT" (Bybit), "BTC" (Hyperliquid).
    double tick_size = 0.0;    // Minimum price increment.
    double lot_size = 0.0;     // Minimum quantity increment.
    Qty min_qty{1};            // Minimum order quantity.

    // Conversions between human-readable numbers and fixed point. Use at the
    // boundaries (configuration, logging, codecs), never in algorithm logic.
    Price price(double px) const { return {std::llround(px / tick_size)}; }
    Qty qty(double q) const { return {std::llround(q / lot_size)}; }
    double to_double(Price p) const { return static_cast<double>(p.ticks) * tick_size; }
    double to_double(Qty q) const { return static_cast<double>(q.lots) * lot_size; }
};

}  // namespace peewee
