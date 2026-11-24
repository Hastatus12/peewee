// ============================================================================
//  ChaseOrder tests: the SPECIFICATION of expected behavior.
//
//  All tests are initially disabled with SKIP_UNTIL_IMPLEMENTED().
//  While implementing src/algo/chase_order.cpp, remove that line from
//  one test at a time (preferably in order) and make it pass.
//
//  Initial book (see sim_harness.hpp): bid 100.0 x 1 | ask 100.5 x 1
//  Tick: 0.1 -> "one tick above 100.0" is 100.1
// ============================================================================

#include "sim_harness.hpp"
#include "peewee/algo/chase_order.hpp"

using namespace peewee;
using namespace peewee::test;
using State = ChaseOrder::State;
using Result = ChaseOrder::Result;

namespace {

ChaseParams buy_params(const SimHarness& h) {
    ChaseParams p;
    p.side = Side::Buy;
    p.qty = h.q(0.01);
    p.limit = h.px(101.0);
    return p;
}

ChaseOrder& start(SimHarness& h, const ChaseParams& p) {
    auto& c = h.engine.start_algo<ChaseOrder>(h.sim, h.btc, p);
    h.pump();
    return c;
}

}  // namespace

// --- 1. Startup ------------------------------------------------------------

TEST(places_post_only_order_at_best_bid) {
    SimHarness h;
    auto& c = start(h, buy_params(h));

    CHECK_EQ(h.sim.count_requests(RequestKind::New), 1u);
    CHECK_EQ(h.sim.requests()[0].px, h.px(100.0));
    CHECK_EQ(h.sim.order(c.order_id())->tif, Tif::PostOnly);
    CHECK_EQ(c.state(), State::Working);
    CHECK_EQ(c.current_price(), h.px(100.0));
}

TEST(waits_for_valid_book_before_placing) {
    SimConfig cfg;
    cfg.latency = 5ms;                       // Snapshot arrives after 5 ms.
    SimHarness h(cfg);
    auto& c = start(h, buy_params(h));

    CHECK_EQ(h.sim.count_requests(RequestKind::New), 0u);
    h.advance(5ms);
    CHECK_EQ(h.sim.count_requests(RequestKind::New), 1u);
    h.advance(5ms);
    CHECK_EQ(c.state(), State::Working);
}

// --- 2. Price calculation --------------------------------------------------

TEST(follows_bid_up) {
    SimHarness h;
    auto& c = start(h, buy_params(h));
    h.advance(100ms);

    h.sim.update_book(h.btc.id, {{h.px(100.2), h.q(1)}}, {});
    h.pump();

    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 1u);
    CHECK_EQ(c.current_price(), h.px(100.2));
}

TEST(never_exceeds_limit) {
    SimHarness h;
    auto p = buy_params(h);
    p.limit = h.px(100.1);
    auto& c = start(h, p);
    h.advance(100ms);

    h.sim.update_book(h.btc.id, {{h.px(100.3), h.q(1)}}, {});
    h.pump();
    CHECK_EQ(c.current_price(), h.px(100.1));

    h.advance(100ms);
    h.sim.update_book(h.btc.id, {{h.px(100.4), h.q(1)}}, {});
    h.pump();
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 1u);  // Already at the limit: no new amend.
}

TEST(does_not_chase_itself) {
    SimHarness h;
    auto p = buy_params(h);
    p.offset_ticks = 1;
    auto& c = start(h, p);
    CHECK_EQ(c.current_price(), h.px(100.1));

    // Our 100.1 is now the highest bid. If we treated it as
    // "other participants' best", we would move to 100.2, then 100.3, ...
    h.advance(500ms);
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 0u);
    CHECK_EQ(c.current_price(), h.px(100.1));
}

// The 1-tick ping-pong seen live on HYPE: the others leave our price, we are
// alone ahead. We must stay (keep priority), and still follow when overtaken.
TEST(never_retreats_when_alone_ahead) {
    SimHarness h;
    auto& c = start(h, buy_params(h));
    CHECK_EQ(c.current_price(), h.px(100.0));
    h.advance(100ms);

    // The others' 100.0 bid goes away: their best is now 99.8, ours alone at 100.0.
    h.sim.set_book(h.btc.id, {{h.px(99.8), h.q(1)}}, {{h.px(100.5), h.q(1)}});
    h.pump();
    h.advance(500ms);
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 0u);  // No retreat to 99.8.
    CHECK_EQ(c.current_price(), h.px(100.0));

    // Someone comes back to 100.0: still nothing to do.
    h.sim.set_book(h.btc.id, {{h.px(100.0), h.q(1)}}, {{h.px(100.5), h.q(1)}});
    h.pump();
    h.advance(500ms);
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 0u);

    // Someone overtakes us at 100.1: follow.
    h.sim.set_book(h.btc.id, {{h.px(100.1), h.q(1)}}, {{h.px(100.5), h.q(1)}});
    h.pump();
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 1u);
    CHECK_EQ(c.current_price(), h.px(100.1));
}

TEST(sell_never_retreats_when_alone_ahead) {
    SimHarness h;
    auto p = buy_params(h);
    p.side = Side::Sell;
    p.limit = h.px(99.0);
    auto& c = start(h, p);
    CHECK_EQ(c.current_price(), h.px(100.5));
    h.advance(100ms);

    // The others' ask moves up to 100.7: we stay at 100.5.
    h.sim.set_book(h.btc.id, {{h.px(100.0), h.q(1)}}, {{h.px(100.7), h.q(1)}});
    h.pump();
    h.advance(500ms);
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 0u);
    CHECK_EQ(c.current_price(), h.px(100.5));
}

TEST(max_move_sets_the_limit_from_the_market) {
    // Book: bid 100.0 / ask 100.5. Buy, no limit, 1% -> limit 100.5 + 1.005 = 101.5.
    SimHarness h;
    auto p = buy_params(h);
    p.limit = Price{};
    p.max_move_pct = 1.0;
    auto& c = start(h, p);
    CHECK_EQ(c.limit(), h.px(101.5));
    CHECK_EQ(c.current_price(), h.px(100.0));  // Still joins the best bid.

    // Sell: from the best bid, 100.0 - 1% = 99.0.
    SimHarness h2;
    auto s = buy_params(h2);
    s.side = Side::Sell;
    s.limit = Price{};
    s.max_move_pct = 1.0;
    auto& c2 = start(h2, s);
    CHECK_EQ(c2.limit(), h2.px(99.0));
}

TEST(max_move_is_ignored_when_a_limit_is_given) {
    SimHarness h;
    auto p = buy_params(h);  // limit 101.0
    p.max_move_pct = 5.0;
    auto& c = start(h, p);
    CHECK_EQ(c.limit(), h.px(101.0));
}

TEST(offset_and_post_only_clamp) {
    SimHarness h;
    auto p = buy_params(h);
    p.offset_ticks = 1;                      // One tick inside the spread.
    auto& c = start(h, p);
    CHECK_EQ(c.current_price(), h.px(100.1));

    // The spread narrows to one tick: 100.3 / 100.4.
    // Target = 100.3 + 1 tick = 100.4, but post-only requires < ask -> 100.3.
    h.advance(100ms);
    h.sim.set_book(h.btc.id, {{h.px(100.3), h.q(1)}}, {{h.px(100.4), h.q(1)}});
    h.pump();
    CHECK_EQ(c.current_price(), h.px(100.3));
}

TEST(sell_side_is_mirrored) {
    SimHarness h;
    ChaseParams p;
    p.side = Side::Sell;
    p.qty = h.q(0.01);
    p.limit = h.px(99.0);
    auto& c = start(h, p);
    CHECK_EQ(c.current_price(), h.px(100.5));

    h.advance(100ms);
    h.sim.update_book(h.btc.id, {}, {{h.px(100.3), h.q(1)}});
    h.pump();
    CHECK_EQ(c.current_price(), h.px(100.3));
}

// --- 3. Request discipline -------------------------------------------------

TEST(only_one_request_in_flight) {
    SimConfig cfg;
    cfg.latency = 5ms;
    SimHarness h(cfg);
    auto p = buy_params(h);
    p.min_reprice_interval = 0ms;
    auto& c = start(h, p);
    h.advance(5ms);                          // Book arrives -> New.
    h.advance(5ms);                          // Ack arrives -> Working.
    CHECK_EQ(c.state(), State::Working);

    h.sim.update_book(h.btc.id, {{h.px(100.2), h.q(1)}}, {});  // Arrives at t+5.
    h.advance(1ms);
    h.sim.update_book(h.btc.id, {{h.px(100.3), h.q(1)}}, {});  // Arrives at t+6.
    h.advance(4ms);                          // t+5: bid 100.2 -> first amend (ack at t+10).
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 1u);

    h.advance(1ms);                          // t+6: bid 100.3 arrives while the amend is still in flight.
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 1u);

    h.advance(4ms);                          // t+10: first amend ack.
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 2u);  // The 100.3 update was not forgotten.

    h.advance(5ms);
    CHECK_EQ(c.current_price(), h.px(100.3));
}

TEST(respects_min_reprice_interval) {
    SimHarness h;
    auto p = buy_params(h);
    p.min_reprice_interval = 50ms;
    auto& c = start(h, p);
    h.advance(100ms);

    h.sim.update_book(h.btc.id, {{h.px(100.1), h.q(1)}}, {});
    h.pump();
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 1u);

    h.advance(10ms);
    h.sim.update_book(h.btc.id, {{h.px(100.2), h.q(1)}}, {});
    h.pump();
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 1u);  // Too early.

    h.advance(45ms);                         // 55 ms have elapsed since the first amend.
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 2u);
    CHECK_EQ(c.current_price(), h.px(100.2));
}

TEST(respects_max_reprices) {
    SimHarness h;
    auto p = buy_params(h);
    p.max_reprices = 1;
    start(h, p);

    h.advance(100ms);
    h.sim.update_book(h.btc.id, {{h.px(100.1), h.q(1)}}, {});
    h.pump();
    h.advance(100ms);
    h.sim.update_book(h.btc.id, {{h.px(100.2), h.q(1)}}, {});
    h.advance(100ms);
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 1u);
}

// --- 4. Executions ---------------------------------------------------------

TEST(partial_then_full_fill) {
    SimHarness h;
    auto& c = start(h, buy_params(h));

    h.sim.trade(h.btc.id, Side::Sell, h.px(100.0), h.q(0.004));
    h.pump();
    CHECK_EQ(c.filled(), h.q(0.004));
    CHECK_EQ(c.remaining(), h.q(0.006));
    CHECK(!c.done());

    h.sim.trade(h.btc.id, Side::Sell, h.px(100.0), h.q(0.006));
    h.pump();
    CHECK(c.done());
    CHECK_EQ(c.result(), Result::Filled);

    // Once finished, no further requests should be sent.
    const size_t n = h.sim.requests().size();
    h.advance(100ms);
    h.sim.update_book(h.btc.id, {{h.px(100.2), h.q(1)}}, {});
    h.advance(100ms);
    CHECK_EQ(h.sim.requests().size(), n);
}

// --- 5. Rejections ---------------------------------------------------------

TEST(retries_after_post_only_reject) {
    SimHarness h;
    h.sim.reject_next(RequestKind::New, RejectReason::PostOnlyWouldCross);
    auto& c = start(h, buy_params(h));

    h.advance(100ms);
    CHECK_EQ(h.sim.count_requests(RequestKind::New), 2u);
    CHECK_EQ(c.state(), State::Working);
}

TEST(gives_up_on_fatal_reject) {
    SimHarness h;
    h.sim.reject_next(RequestKind::New, RejectReason::InsufficientBalance);
    auto& c = start(h, buy_params(h));

    CHECK(c.done());
    CHECK_EQ(c.result(), Result::Rejected);
    h.advance(100ms);
    CHECK_EQ(h.sim.count_requests(RequestKind::New), 1u);
}

// An amend or cancel reject does not close the order (see OrderReject in
// events.hpp): the chase must keep managing it. Seen live on Bybit:
// "order not modified" ended the chase and left the order in the book.
TEST(amend_reject_keeps_working) {
    SimHarness h;
    auto& c = start(h, buy_params(h));
    h.advance(100ms);

    h.sim.reject_next(RequestKind::Amend, RejectReason::Unknown);
    h.sim.update_book(h.btc.id, {{h.px(100.2), h.q(1)}}, {});
    h.pump();
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 1u);
    CHECK(!c.done());
    CHECK(h.sim.order(c.order_id())->open);

    h.advance(100ms);  // Retries once min_reprice_interval has passed.
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 2u);
    CHECK_EQ(c.current_price(), h.px(100.2));
}

// Seen live on Hyperliquid (2026-11-22, HYPE): a modify refused as post-only
// also CANCELED the order, and the chase ended with nothing filled. It must
// place a new order instead.
TEST(restarts_when_a_refused_amend_cancels_the_order) {
    SimConfig cfg;
    cfg.amend_reject_cancels = true;  // Like Hyperliquid.
    SimHarness h(cfg);
    auto& c = start(h, buy_params(h));
    h.advance(100ms);
    const ClientOrderId first = c.order_id();

    h.sim.reject_next(RequestKind::Amend, RejectReason::PostOnlyWouldCross);
    h.sim.update_book(h.btc.id, {{h.px(100.2), h.q(1)}}, {});
    h.pump();

    CHECK(!c.done());
    CHECK(!h.sim.order(first)->open);       // The exchange removed it...
    CHECK(c.order_id() != first);           // ...so the chase placed a new one,
    CHECK(h.sim.order(c.order_id())->open);
    CHECK_EQ(h.sim.order(c.order_id())->px, h.px(100.2));  // at the current best bid,
    CHECK_EQ(h.sim.order(c.order_id())->qty, h.q(0.01));   // for the whole quantity.
    CHECK_EQ(c.state(), State::Working);
}

TEST(restart_after_partial_fill_is_for_the_rest_only) {
    SimConfig cfg;
    cfg.amend_reject_cancels = true;
    SimHarness h(cfg);
    auto& c = start(h, buy_params(h));
    h.advance(100ms);

    h.sim.trade(h.btc.id, Side::Sell, h.px(100.0), h.q(0.004));  // Partial fill.
    h.pump();
    CHECK_EQ(c.filled(), h.q(0.004));

    h.sim.reject_next(RequestKind::Amend, RejectReason::PostOnlyWouldCross);
    h.sim.update_book(h.btc.id, {{h.px(100.2), h.q(1)}}, {});
    h.pump();

    CHECK(!c.done());
    CHECK_EQ(h.sim.order(c.order_id())->qty, h.q(0.006));
}

TEST(stop_still_finishes_canceled) {
    // A cancel WE asked for must end the chase, not restart it.
    SimConfig cfg;
    cfg.amend_reject_cancels = true;
    SimHarness h(cfg);
    auto& c = start(h, buy_params(h));
    c.stop();
    h.pump();
    CHECK(c.done());
    CHECK_EQ(c.result(), Result::Canceled);
    CHECK_EQ(h.sim.count_requests(RequestKind::New), 1u);
}

TEST(cancel_reject_retries_cancel) {
    SimHarness h;
    auto& c = start(h, buy_params(h));

    h.sim.reject_next(RequestKind::Cancel, RejectReason::Unknown);
    c.stop();
    h.pump();
    CHECK(!c.done());
    CHECK(h.sim.order(c.order_id())->open);

    h.advance(100ms);
    CHECK_EQ(h.sim.count_requests(RequestKind::Cancel), 2u);
    CHECK(c.done());
    CHECK_EQ(c.result(), Result::Canceled);
    CHECK(!h.sim.order(c.order_id())->open);
}

TEST(finishes_when_status_shows_missed_fill) {
    SimHarness h;
    auto& c = start(h, buy_params(h));

    h.sim.set_private_stream(false);         // The fill is lost...
    h.sim.trade(h.btc.id, Side::Sell, h.px(100.0), h.q(0.01));
    h.pump();
    CHECK(!c.done());

    h.sim.set_private_stream(true);          // ...and found by the reconciliation query.
    h.advance(100ms);
    CHECK(c.done());
    CHECK_EQ(c.result(), Result::Filled);
    CHECK_EQ(c.filled(), h.q(0.01));
}

// --- 6. Invalid book -------------------------------------------------------

TEST(does_not_act_on_invalid_book) {
    SimHarness h;
    auto& c = start(h, buy_params(h));
    h.advance(100ms);

    h.sim.set_public_stream(false);
    h.sim.update_book(h.btc.id, {{h.px(100.2), h.q(1)}}, {});  // Not published: stream down.
    h.advance(100ms);
    CHECK_EQ(h.sim.count_requests(RequestKind::Amend), 0u);

    h.sim.set_public_stream(true);           // Stream returns and sends a snapshot with bid 100.2.
    h.advance(100ms);
    CHECK_EQ(c.current_price(), h.px(100.2));
}

// --- 7. Early termination --------------------------------------------------

TEST(stop_cancels_order) {
    SimHarness h;
    auto& c = start(h, buy_params(h));

    c.stop();
    h.pump();
    CHECK_EQ(h.sim.count_requests(RequestKind::Cancel), 1u);
    CHECK(c.done());
    CHECK_EQ(c.result(), Result::Canceled);
}

TEST(expires_after_max_duration) {
    SimHarness h;
    auto p = buy_params(h);
    p.max_duration = 1s;
    auto& c = start(h, p);

    h.advance(500ms);
    CHECK(!c.done());
    h.advance(600ms);
    CHECK_EQ(h.sim.count_requests(RequestKind::Cancel), 1u);
    CHECK(c.done());
    CHECK_EQ(c.result(), Result::Expired);
}

TEST_MAIN()
