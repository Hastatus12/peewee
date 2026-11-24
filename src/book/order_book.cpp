#include "peewee/book/order_book.hpp"

#include <algorithm>

namespace peewee {

bool OrderBook::apply(const BookDelta& d) {
    if (d.snapshot) {
        bids_.clear();
        asks_.clear();
    } else {
        if (!valid_) return false;      // Wait for a snapshot.
        if (d.seq <= seq_) return false; // Stale or duplicate message.
    }

    for (const Level& l : d.bids) set_level(bids_, l, /*descending=*/true);
    for (const Level& l : d.asks) set_level(asks_, l, /*descending=*/false);

    seq_ = d.seq;
    ts_ = d.ts;
    valid_ = true;
    return true;
}

void OrderBook::invalidate() {
    valid_ = false;
    bids_.clear();
    asks_.clear();
}

std::optional<Level> OrderBook::best(Side side) const {
    const auto& lv = levels(side);
    if (lv.empty()) return std::nullopt;
    return lv.front();
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.front().px;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.front().px;
}

Qty OrderBook::qty_at(Side side, Price px) const {
    for (const Level& l : levels(side)) {
        if (l.px == px) return l.qty;
    }
    return {};
}

void OrderBook::set_level(std::vector<Level>& lv, Level l, bool descending) {
    // Position of the first level that does not precede l.px.
    auto it = std::lower_bound(lv.begin(), lv.end(), l.px, [descending](const Level& a, Price p) {
        return descending ? a.px > p : a.px < p;
    });
    const bool exists = it != lv.end() && it->px == l.px;

    if (l.qty.is_zero()) {
        if (exists) lv.erase(it);
    } else if (exists) {
        it->qty = l.qty;
    } else {
        lv.insert(it, l);
    }
}

}  // namespace peewee
