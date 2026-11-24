#include <vector>

#include "check.hpp"
#include "peewee/oms/order_manager.hpp"

using namespace peewee;
using namespace std::chrono_literals;

namespace {

// Fake venue that only records requests.
class RecordingVenue final : public IVenue {
public:
    VenueId id() const override { return 0; }
    std::string_view name() const override { return "recording"; }
    const VenueCapabilities& caps() const override { return caps_; }
    void set_sink(IEventSink*) override {}
    void add_instrument(const Instrument&) override {}
    void connect() override {}
    void disconnect() override {}
    void subscribe_book(InstrumentId, int) override {}
    void subscribe_private() override {}
    void send_new(const NewOrder& o) override { news.push_back(o); }
    void send_amend(const AmendOrder& o) override { amends.push_back(o); }
    void send_cancel(const CancelOrder& o) override { cancels.push_back(o); }
    void query_order(const QueryOrder& q) override { queries.push_back(q); }
    void resync() override {}
    bool can_send(RequestKind) const override { return true; }

    std::vector<NewOrder> news;
    std::vector<AmendOrder> amends;
    std::vector<CancelOrder> cancels;
    std::vector<QueryOrder> queries;

private:
    VenueCapabilities caps_;
};

// An OMS with one acknowledged (Live) order of 10 lots at 100.
struct LiveOrder {
    LiveOrder() : oms(clock, {.request_timeout = 1s}) {
        cid = oms.send_new(0, v, 0, Side::Buy, Price{100}, Qty{10}, Tif::GTC);
        oms.on_event(OrderAck{cid, v.news.back().req, "X1", Price{100}, Qty{10}, {}});
    }
    const Order& order() const { return *oms.find(cid); }

    ManualClock clock;
    RecordingVenue v;
    OrderManager oms;
    ClientOrderId cid = kNoOrder;
};

}  // namespace

TEST(new_then_ack_goes_live) {
    ManualClock clock;
    RecordingVenue v;
    OrderManager oms(clock);
    const auto cid = oms.send_new(7, v, 0, Side::Buy, Price{100}, Qty{10}, Tif::PostOnly);

    CHECK_EQ(v.news.size(), 1u);
    CHECK_EQ(v.news[0].cid, cid);
    CHECK(v.news[0].req != kNoRequest);
    CHECK_EQ(oms.find(cid)->state, OrderState::PendingNew);
    CHECK(oms.find(cid)->has_pending());

    oms.on_event(OrderAck{cid, v.news[0].req, "X1", Price{100}, Qty{10}, {}});
    CHECK_EQ(oms.find(cid)->state, OrderState::Live);
    CHECK(!oms.find(cid)->has_pending());
    CHECK_EQ(oms.find(cid)->owner, 7u);
}

TEST(partial_and_full_fill) {
    LiveOrder t;
    t.oms.on_event(Fill{t.cid, "E1", Price{100}, Qty{4}, true, {}});
    CHECK_EQ(t.order().remaining(), Qty{6});
    CHECK_EQ(t.order().state, OrderState::Live);

    t.oms.on_event(Fill{t.cid, "E2", Price{100}, Qty{6}, true, {}});
    CHECK_EQ(t.order().state, OrderState::Filled);
    CHECK(!t.oms.send_amend(t.cid, Price{101}));  // Order closed.
}

TEST(order_stays_live_while_amend_is_pending) {
    LiveOrder t;
    CHECK(t.oms.send_amend(t.cid, Price{101}));
    CHECK_EQ(t.order().state, OrderState::Live);   // Still in the book...
    CHECK(t.order().has_pending());                 // ...with an amend in progress.
    CHECK_EQ(t.order().pending->kind, RequestKind::Amend);
    CHECK_EQ(t.order().pending->px, Price{101});

    t.oms.on_event(Fill{t.cid, "E1", Price{100}, Qty{3}, true, {}});  // Filled meanwhile.
    CHECK_EQ(t.order().filled, Qty{3});
    CHECK(t.order().has_pending());

    t.oms.on_event(AmendAck{t.cid, t.v.amends.back().req, Price{101}, Qty{10}, {}});
    CHECK_EQ(t.order().px, Price{101});
    CHECK(!t.order().has_pending());
}

TEST(only_one_request_at_a_time) {
    LiveOrder t;
    CHECK(t.oms.send_amend(t.cid, Price{101}));
    CHECK(!t.oms.send_amend(t.cid, Price{102}));   // Refused: amend already pending.
    CHECK(!t.oms.send_cancel(t.cid));              // Refused as well.
    CHECK_EQ(t.v.amends.size(), 1u);
    CHECK_EQ(t.v.cancels.size(), 0u);

    t.oms.on_event(AmendAck{t.cid, t.v.amends.back().req, Price{101}, Qty{10}, {}});
    CHECK(t.oms.send_cancel(t.cid));               // Free again.
}

TEST(amend_reject_keeps_order_unchanged) {
    LiveOrder t;
    CHECK(t.oms.send_amend(t.cid, Price{101}));
    t.oms.on_event(OrderReject{t.cid, t.v.amends.back().req, RequestKind::Amend,
                               RejectReason::PostOnlyWouldCross, "", {}});
    CHECK_EQ(t.order().state, OrderState::Live);
    CHECK_EQ(t.order().px, Price{100});
    CHECK(!t.order().has_pending());
}

TEST(stale_response_is_dropped) {
    LiveOrder t;
    int calls = 0;
    t.oms.set_listener([&](const Order&, const OrderEvent&) { ++calls; });

    CHECK(t.oms.send_amend(t.cid, Price{101}));
    const RequestId current = t.v.amends.back().req;
    t.oms.on_event(AmendAck{t.cid, current + 999, Price{105}, Qty{10}, {}});  // Not ours.
    CHECK_EQ(t.order().px, Price{100});
    CHECK(t.order().has_pending());
    CHECK_EQ(calls, 0);

    t.oms.on_event(AmendAck{t.cid, current, Price{101}, Qty{10}, {}});
    CHECK_EQ(t.order().px, Price{101});
    CHECK_EQ(calls, 1);
}

// Seen live on Hyperliquid (HYPE, 2026-11-23): the "open" of a new order
// brings OrderAck + AmendAck (no request ID). The chase sent an amend right
// after the OrderAck, and the AmendAck of the SAME message was taken as the
// answer to that amend: the OMS believed it done, and a second amend left
// while the first was still in flight.
TEST(ack_without_id_answers_amend_only_with_requested_price) {
    LiveOrder t;
    CHECK(t.oms.send_amend(t.cid, Price{101}));

    // Old news: the order at its previous price, no request ID.
    t.oms.on_event(AmendAck{t.cid, kNoRequest, Price{100}, Qty{10}, {}});
    CHECK(t.order().has_pending());               // Still waiting for OUR amend...
    CHECK_EQ(t.order().px, Price{100});
    CHECK(!t.oms.send_amend(t.cid, Price{102}));  // ...so no second amend can leave.

    // The real answer: the price we asked for.
    t.oms.on_event(AmendAck{t.cid, kNoRequest, Price{101}, Qty{10}, {}});
    CHECK(!t.order().has_pending());
    CHECK_EQ(t.order().px, Price{101});
}

TEST(duplicate_fill_is_dropped) {
    LiveOrder t;
    int fills = 0;
    t.oms.set_listener([&](const Order&, const OrderEvent& e) { fills += std::holds_alternative<Fill>(e); });

    t.oms.on_event(Fill{t.cid, "E1", Price{100}, Qty{4}, true, {}});
    t.oms.on_event(Fill{t.cid, "E1", Price{100}, Qty{4}, true, {}});  // Same execution again.
    CHECK_EQ(t.order().filled, Qty{4});
    CHECK_EQ(fills, 1);
}

TEST(timeout_makes_outcome_unknown_and_queries) {
    LiveOrder t;
    CHECK(t.oms.send_amend(t.cid, Price{101}));

    t.clock.advance(900ms);
    t.oms.check_timeouts();
    CHECK(t.v.queries.empty());

    t.clock.advance(200ms);                        // 1.1 s > request_timeout.
    t.oms.check_timeouts();
    CHECK_EQ(t.v.queries.size(), 1u);
    CHECK(t.order().pending->unknown);

    t.clock.advance(1s);                           // No answer to the query either: ask again.
    t.oms.check_timeouts();
    CHECK_EQ(t.v.queries.size(), 2u);

    // The exchange says: the amend was applied and 2 lots were filled.
    t.oms.on_event(OrderStatus{t.cid, true, true, Price{101}, Qty{10}, Qty{2}, {}});
    CHECK(!t.order().has_pending());
    CHECK_EQ(t.order().px, Price{101});
    CHECK_EQ(t.order().filled, Qty{2});
    CHECK_EQ(t.order().state, OrderState::Live);
}

TEST(order_not_found_triggers_query) {
    LiveOrder t;
    CHECK(t.oms.send_cancel(t.cid));
    t.oms.on_event(OrderReject{t.cid, t.v.cancels.back().req, RequestKind::Cancel,
                               RejectReason::OrderNotFound, "", {}});
    CHECK_EQ(t.v.queries.size(), 1u);

    t.oms.on_event(OrderStatus{t.cid, true, false, Price{100}, Qty{10}, Qty{10}, {}});
    CHECK_EQ(t.order().state, OrderState::Filled);
}

TEST(reconcile_queries_open_orders_only) {
    LiveOrder t;
    const auto closed = t.oms.send_new(0, t.v, 0, Side::Buy, Price{90}, Qty{1}, Tif::GTC);
    t.oms.on_event(OrderClosed{closed, kNoRequest, CloseReason::Canceled, {}});

    t.oms.reconcile_venue(t.v.id());
    CHECK_EQ(t.v.queries.size(), 1u);
    CHECK_EQ(t.v.queries[0].cid, t.cid);
}

TEST(listener_receives_events) {
    LiveOrder t;
    int calls = 0;
    t.oms.set_listener([&](const Order&, const OrderEvent&) { ++calls; });
    CHECK(t.oms.send_cancel(t.cid));
    t.oms.on_event(OrderClosed{t.cid, t.v.cancels.back().req, CloseReason::Canceled, {}});
    CHECK_EQ(calls, 1);
    CHECK_EQ(t.order().state, OrderState::Canceled);
    CHECK(!t.order().has_pending());
}

TEST_MAIN()
