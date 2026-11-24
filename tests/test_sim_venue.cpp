// Verify that the simulator and engine behave as expected,
// using a minimal probe algorithm.

#include <algorithm>

#include "sim_harness.hpp"

using namespace peewee;
using namespace peewee::test;

namespace {

// Probe algorithm: record received events and place orders on request.
class ProbeAlgo final : public Algo {
public:
    using Algo::Algo;

    void on_book(const OrderBook& b) override {
        ++books;
        last_bid = b.best_bid();
    }
    void on_order(const Order& o, const OrderEvent& e) override {
        events.push_back(e);
        last_state = o.state;
    }
    bool done() const override { return false; }

    ClientOrderId place(Side side, Price px, Qty qty, Tif tif) {
        return ctx_.oms.send_new(id(), ctx_.venue, ctx_.instrument.id, side, px, qty, tif);
    }

    ClientOrderId place(NewOrder order) {
        order.inst = ctx_.instrument.id;
        return ctx_.oms.send_new(id(), ctx_.venue, order);
    }

    int books = 0;
    std::optional<Price> last_bid;
    std::vector<OrderEvent> events;
    OrderState last_state = OrderState::PendingNew;
};

}  // namespace

TEST(book_is_delivered_after_subscribe) {
    SimHarness h;
    auto& a = h.engine.start_algo<ProbeAlgo>(h.sim, h.btc);
    h.pump();
    CHECK_EQ(a.books, 1);
    CHECK_EQ(*a.last_bid, h.px(100.0));
    CHECK(h.engine.book(h.btc.id).valid());
}

TEST(own_order_appears_in_book) {
    SimHarness h;
    auto& a = h.engine.start_algo<ProbeAlgo>(h.sim, h.btc);
    h.pump();
    a.place(Side::Buy, h.px(100.1), h.q(0.5), Tif::PostOnly);
    h.pump();
    CHECK_EQ(a.last_state, OrderState::Live);
    CHECK_EQ(*a.last_bid, h.px(100.1));
    CHECK_EQ(h.engine.book(h.btc.id).qty_at(Side::Buy, h.px(100.1)), h.q(0.5));
}

// Book: bid 100.0, ask 100.5.

TEST(market_order_fills_within_its_worst_price) {
    SimHarness h;
    auto& a = h.engine.start_algo<ProbeAlgo>(h.sim, h.btc);
    h.pump();
    a.place(NewOrder{.side = Side::Buy, .type = OrderType::Market, .px = h.px(101.0), .qty = h.q(0.5)});
    h.pump();
    CHECK_EQ(a.last_state, OrderState::Filled);
    const auto fill = std::find_if(a.events.begin(), a.events.end(),
                                   [](const OrderEvent& e) { return std::holds_alternative<Fill>(e); });
    CHECK(fill != a.events.end());
    CHECK_EQ(std::get<Fill>(*fill).px, h.px(100.5));  // At the ask, not at the worst price.
}

TEST(market_order_beyond_the_book_is_canceled_not_rested) {
    SimHarness h;
    auto& a = h.engine.start_algo<ProbeAlgo>(h.sim, h.btc);
    h.pump();
    // Worst price below the ask: nothing to buy, and a market order never rests.
    a.place(NewOrder{.side = Side::Buy, .type = OrderType::Market, .px = h.px(100.2), .qty = h.q(0.5)});
    h.pump();
    CHECK_EQ(a.last_state, OrderState::Canceled);
}

TEST(stop_waits_out_of_the_book_until_triggered) {
    SimHarness h;
    auto& a = h.engine.start_algo<ProbeAlgo>(h.sim, h.btc);
    h.pump();
    // Stop-loss of a long: sell at market (worst 98.0) if the price falls to 99.0.
    const auto cid = a.place(NewOrder{.side = Side::Sell, .type = OrderType::StopMarket, .px = h.px(98.0),
                                      .qty = h.q(0.5), .trigger_px = h.px(99.0), .reduce_only = true});
    h.pump();
    CHECK_EQ(a.last_state, OrderState::Live);
    CHECK(h.sim.order(cid)->waiting_trigger);
    CHECK_EQ(h.engine.book(h.btc.id).qty_at(Side::Sell, h.px(98.0)), Qty{});  // Not in the book.

    h.sim.trade(h.btc.id, Side::Sell, h.px(99.5), h.q(0.1));  // Above the trigger: nothing.
    h.pump();
    CHECK(h.sim.order(cid)->waiting_trigger);

    h.sim.set_book(h.btc.id, {{h.px(98.9), h.q(1)}}, {{h.px(99.1), h.q(1)}});  // Mid 99.0: trigger.
    h.pump();
    CHECK_EQ(a.last_state, OrderState::Filled);
    CHECK_EQ(std::get<Fill>(*std::find_if(a.events.begin(), a.events.end(), [](const OrderEvent& e) {
                 return std::holds_alternative<Fill>(e);
             })).px,
             h.px(98.9));
}

TEST(take_profit_limit_rests_after_trigger) {
    SimHarness h;
    auto& a = h.engine.start_algo<ProbeAlgo>(h.sim, h.btc);
    h.pump();
    // Take-profit of a long: sell limit 101.5 once the price rises to 101.0.
    const auto cid = a.place(NewOrder{.side = Side::Sell, .type = OrderType::TakeProfitLimit, .px = h.px(101.5),
                                      .qty = h.q(0.5), .trigger_px = h.px(101.0)});
    h.pump();
    CHECK(h.sim.order(cid)->waiting_trigger);

    h.sim.trade(h.btc.id, Side::Buy, h.px(101.0), h.q(0.1));
    h.pump();
    CHECK(!h.sim.order(cid)->waiting_trigger);
    CHECK_EQ(a.last_state, OrderState::Live);  // Now resting in the book at 101.5.
    CHECK_EQ(h.engine.book(h.btc.id).qty_at(Side::Sell, h.px(101.5)), h.q(0.5));
}

TEST(post_only_crossing_is_rejected) {
    SimHarness h;
    auto& a = h.engine.start_algo<ProbeAlgo>(h.sim, h.btc);
    h.pump();
    a.place(Side::Buy, h.px(100.5), h.q(0.5), Tif::PostOnly);
    h.pump();
    CHECK_EQ(a.last_state, OrderState::Rejected);
    const auto& r = std::get<OrderReject>(a.events.back());
    CHECK_EQ(r.reason, RejectReason::PostOnlyWouldCross);
}

TEST(trade_fills_resting_order) {
    SimHarness h;
    auto& a = h.engine.start_algo<ProbeAlgo>(h.sim, h.btc);
    h.pump();
    a.place(Side::Buy, h.px(100.0), h.q(0.5), Tif::PostOnly);
    h.pump();
    h.sim.trade(h.btc.id, Side::Sell, h.px(100.0), h.q(0.2));
    h.pump();
    CHECK(std::holds_alternative<Fill>(a.events.back()));
    h.sim.trade(h.btc.id, Side::Sell, h.px(99.0), h.q(5));
    h.pump();
    CHECK_EQ(a.last_state, OrderState::Filled);
}

TEST(latency_delays_responses) {
    SimConfig cfg;
    cfg.latency = 5ms;
    SimHarness h(cfg);
    auto& a = h.engine.start_algo<ProbeAlgo>(h.sim, h.btc);
    h.pump();
    CHECK_EQ(a.books, 0);          // Snapshot has not arrived yet.
    h.advance(5ms);
    CHECK_EQ(a.books, 1);
    a.place(Side::Buy, h.px(100.0), h.q(0.5), Tif::PostOnly);
    h.advance(4ms);
    CHECK(a.events.empty());       // Ack has not arrived yet.
    h.advance(1ms);
    CHECK_EQ(a.last_state, OrderState::Live);
}

TEST(public_stream_down_invalidates_book) {
    SimHarness h;
    h.engine.start_algo<ProbeAlgo>(h.sim, h.btc);
    h.pump();
    h.sim.set_public_stream(false);
    h.pump();
    CHECK(!h.engine.book(h.btc.id).valid());
    h.sim.set_public_stream(true);
    h.pump();
    CHECK(h.engine.book(h.btc.id).valid());
}

// --- Robustness ------------------------------------------------------------

TEST(duplicate_fill_reaches_algo_once) {
    SimHarness h;
    auto& a = h.engine.start_algo<ProbeAlgo>(h.sim, h.btc);
    h.pump();
    const auto cid = a.place(Side::Buy, h.px(100.0), h.q(0.5), Tif::PostOnly);
    h.pump();
    h.sim.trade(h.btc.id, Side::Sell, h.px(100.0), h.q(0.2));
    h.pump();
    h.sim.replay_last_fill();
    h.pump();

    const auto fills = std::count_if(a.events.begin(), a.events.end(),
                                     [](const OrderEvent& e) { return std::holds_alternative<Fill>(e); });
    CHECK_EQ(fills, 1);
    CHECK_EQ(h.engine.oms().find(cid)->filled, h.q(0.2));
}

TEST(lost_response_is_recovered_by_query) {
    Engine::Options opt;
    opt.request_timeout = 100ms;
    SimHarness h({}, opt);
    auto& a = h.engine.start_algo<ProbeAlgo>(h.sim, h.btc);
    h.pump();
    const auto cid = a.place(Side::Buy, h.px(100.0), h.q(0.5), Tif::PostOnly);
    h.pump();

    h.sim.drop_next_response(RequestKind::Amend);
    CHECK(h.engine.oms().send_amend(cid, h.px(100.2)));
    h.advance(50ms);
    CHECK(h.engine.oms().find(cid)->has_pending());    // No answer yet.

    h.advance(60ms);                                   // Timeout: the OMS asks the exchange.
    CHECK_EQ(h.sim.query_count(), 1u);
    CHECK(!h.engine.oms().find(cid)->has_pending());
    CHECK_EQ(h.engine.oms().find(cid)->px, h.px(100.2));  // The amend had been applied.
    CHECK(std::holds_alternative<OrderStatus>(a.events.back()));
}

TEST(fills_missed_while_private_stream_down_are_recovered) {
    SimHarness h;
    auto& a = h.engine.start_algo<ProbeAlgo>(h.sim, h.btc);
    h.pump();
    const auto cid = a.place(Side::Buy, h.px(100.0), h.q(0.5), Tif::PostOnly);
    h.pump();

    h.sim.set_private_stream(false);
    h.sim.trade(h.btc.id, Side::Sell, h.px(100.0), h.q(0.5));  // This fill is lost.
    h.pump();
    CHECK_EQ(h.engine.oms().find(cid)->state, OrderState::Live);

    h.sim.set_private_stream(true);                   // Reconnect -> reconciliation.
    h.pump();
    CHECK_EQ(h.engine.oms().find(cid)->state, OrderState::Filled);
    CHECK_EQ(h.engine.oms().find(cid)->filled, h.q(0.5));
    CHECK_EQ(a.last_state, OrderState::Filled);
}

TEST(queue_overflow_triggers_resync) {
    Engine::Options opt;
    opt.queue_capacity = 4;
    SimHarness h({}, opt);
    auto& a = h.engine.start_algo<ProbeAlgo>(h.sim, h.btc);
    h.pump();
    CHECK(h.engine.book(h.btc.id).valid());

    // Ten book updates delivered in one burst: more than the queue can hold.
    for (int i = 1; i <= 10; ++i) h.sim.update_book(h.btc.id, {{h.px(100.1), h.q(i)}}, {});
    h.engine.poll();                                  // Venue fills the queue and overflows.
    h.pump();                                         // Engine notices, resyncs, gets snapshots.

    CHECK(h.engine.book(h.btc.id).valid());
    CHECK_EQ(h.engine.book(h.btc.id).qty_at(Side::Buy, h.px(100.1)), h.q(10));  // Latest state, not a stale one.
    CHECK(a.books >= 2);
}

TEST(signal_safe_stop_request) {
    SimHarness h;
    struct StoppableAlgo final : Algo {
        using Algo::Algo;
        void stop() override { stopped = true; }
        bool done() const override { return stopped; }
        bool stopped = false;
    };
    auto& a = h.engine.start_algo<StoppableAlgo>(h.sim, h.btc);
    h.engine.request_stop_algos();                    // Only sets a flag...
    CHECK(!a.stopped);
    h.pump();                                         // ...the engine loop does the stopping.
    CHECK(a.stopped);
}

TEST_MAIN()
