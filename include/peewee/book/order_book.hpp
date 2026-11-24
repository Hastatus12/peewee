#pragma once
// Local copy of an instrument's order book.

#include <optional>
#include <vector>

#include "peewee/core/events.hpp"
#include "peewee/core/types.hpp"

namespace peewee {

class OrderBook {
public:
    explicit OrderBook(InstrumentId inst = 0) : inst_(inst) {}

    // Apply a snapshot or delta.
    // - Ignore deltas received while the book is invalid
    //   (wait for a new snapshot).
    // - Ignore stale deltas with seq <= the last observed sequence.
    // Return true if the book changed.
    //
    // Sequence-gap detection belongs to the adapter because
    // each exchange has different rules: on a gap, call invalidate()
    // (through VenueStatus) and request a new snapshot.
    bool apply(const BookDelta& d);

    // Mark the book as unreliable (e.g. after a disconnect).
    void invalidate();

    bool valid() const { return valid_; }
    InstrumentId instrument() const { return inst_; }
    uint64_t seq() const { return seq_; }
    Timestamp last_update() const { return ts_; }

    // Side::Buy = bid side (buyers), Side::Sell = ask side (sellers).
    std::optional<Level> best(Side side) const;
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    // Total displayed quantity at this price (0 if the level does not exist).
    Qty qty_at(Side side, Price px) const;

    // Level count and indexed access: 0 = best level.
    size_t depth(Side side) const { return levels(side).size(); }
    const Level& level(Side side, size_t i) const { return levels(side)[i]; }

private:
    const std::vector<Level>& levels(Side s) const { return s == Side::Buy ? bids_ : asks_; }
    static void set_level(std::vector<Level>& lv, Level l, bool descending);

    InstrumentId inst_;
    bool valid_ = false;
    uint64_t seq_ = 0;
    Timestamp ts_{};
    // Sorted vectors: bids from highest price, asks from lowest.
    // For 50-200 levels, these are faster than std::map (contiguous memory).
    std::vector<Level> bids_;
    std::vector<Level> asks_;
};

}  // namespace peewee
