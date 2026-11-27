#pragma once
// Description of a tradable instrument (e.g. BTCUSDT perpetual on Bybit).

#include <algorithm>
#include <cmath>
#include <cstdint>
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

    // Some exchanges also limit a price's significant digits (Hyperliquid: 5).
    // Then not every multiple of tick_size is a valid price: with 5, 12345.6
    // is refused while 1234.5 and 12345 are fine. 0 = no such limit.
    int price_sig_figs = 0;

    // Conversions between human-readable numbers and fixed point. Use at the
    // boundaries (configuration, logging, codecs), never in algorithm logic.
    Price price(double px) const { return {std::llround(px / tick_size)}; }
    Qty qty(double q) const { return {std::llround(q / lot_size)}; }
    double to_double(Price p) const { return static_cast<double>(p.ticks) * tick_size; }
    double to_double(Qty q) const { return static_cast<double>(q.lots) * lot_size; }

    // --- Valid prices ---
    // A price is valid if it is a multiple of the tick (always true for a
    // Price) and has at most price_sig_figs significant digits. Whole prices
    // are always valid, as on Hyperliquid. Algorithms round in the safe
    // direction: a buy down, a sell up, so rounding never pays more.

    // Distance between two neighboring valid prices around p, in ticks.
    int64_t price_step(Price p) const {
        if (price_sig_figs <= 0 || p.ticks <= 0) return 1;
        int digits = 0;
        for (int64_t n = p.ticks; n > 0; n /= 10) ++digits;
        int64_t step = 1;
        for (int i = price_sig_figs; i < digits; ++i) step *= 10;
        const int64_t ticks_per_unit = std::max<int64_t>(1, std::llround(1.0 / tick_size));
        return std::min(step, ticks_per_unit);  // A whole price is always valid.
    }

    bool is_valid(Price p) const { return p.ticks % price_step(p) == 0; }

    // The largest valid price <= p.
    Price round_down(Price p) const {
        const int64_t step = price_step(p);
        return {p.ticks - p.ticks % step};
    }

    // The smallest valid price >= p.
    Price round_up(Price p) const {
        const int64_t step = price_step(p);
        const int64_t rest = p.ticks % step;
        return rest == 0 ? p : Price{p.ticks + step - rest};
    }

    // The next valid price strictly above / below p: "one tick" in the
    // exchange's terms, whatever the price.
    Price step_up(Price p) const { return round_up(p + 1); }
    Price step_down(Price p) const { return round_down(p - 1); }
};

}  // namespace peewee
