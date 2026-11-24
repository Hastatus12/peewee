#include "check.hpp"
#include "peewee/book/order_book.hpp"

using namespace peewee;

namespace {

BookDelta snapshot(uint64_t seq, std::vector<Level> bids, std::vector<Level> asks) {
    return BookDelta{.inst = 0, .seq = seq, .snapshot = true, .ts = {}, .bids = std::move(bids), .asks = std::move(asks)};
}

BookDelta delta(uint64_t seq, std::vector<Level> bids, std::vector<Level> asks) {
    return BookDelta{.inst = 0, .seq = seq, .snapshot = false, .ts = {}, .bids = std::move(bids), .asks = std::move(asks)};
}

}  // namespace

TEST(empty_book_is_invalid) {
    OrderBook b;
    CHECK(!b.valid());
    CHECK(!b.best_bid());
    CHECK(!b.best_ask());
}

TEST(snapshot_sorts_levels) {
    OrderBook b;
    b.apply(snapshot(1, {{{99}, {1}}, {{100}, {2}}, {{98}, {3}}},
                        {{{102}, {1}}, {{101}, {2}}}));
    CHECK(b.valid());
    CHECK_EQ(*b.best_bid(), Price{100});
    CHECK_EQ(*b.best_ask(), Price{101});
    CHECK_EQ(b.level(Side::Buy, 2).px, Price{98});
    CHECK_EQ(b.qty_at(Side::Buy, Price{99}), Qty{1});
}

TEST(delta_updates_inserts_and_removes) {
    OrderBook b;
    b.apply(snapshot(1, {{{100}, {1}}}, {{{101}, {1}}}));
    b.apply(delta(2, {{{100}, {5}}, {{99}, {2}}}, {{{101}, {0}}, {{103}, {1}}}));
    CHECK_EQ(b.qty_at(Side::Buy, Price{100}), Qty{5});
    CHECK_EQ(b.depth(Side::Buy), 2u);
    CHECK_EQ(*b.best_ask(), Price{103});
    CHECK_EQ(b.depth(Side::Sell), 1u);
}

TEST(stale_delta_is_ignored) {
    OrderBook b;
    b.apply(snapshot(5, {{{100}, {1}}}, {}));
    CHECK(!b.apply(delta(5, {{{100}, {9}}}, {})));
    CHECK(!b.apply(delta(4, {{{100}, {9}}}, {})));
    CHECK_EQ(b.qty_at(Side::Buy, Price{100}), Qty{1});
}

TEST(delta_before_snapshot_is_ignored) {
    OrderBook b;
    CHECK(!b.apply(delta(1, {{{100}, {1}}}, {})));
    CHECK(!b.valid());
}

TEST(invalidate_clears_book) {
    OrderBook b;
    b.apply(snapshot(1, {{{100}, {1}}}, {{{101}, {1}}}));
    b.invalidate();
    CHECK(!b.valid());
    CHECK(!b.best_bid());
    CHECK(!b.apply(delta(2, {{{100}, {1}}}, {})));
}

TEST_MAIN()
