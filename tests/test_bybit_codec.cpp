#include <fstream>
#include <sstream>

#include "bybit_auth.hpp"
#include "bybit_codec.hpp"
#include "check.hpp"

using namespace peewee;

namespace {

Instrument btc() {
    Instrument i;
    i.symbol = "BTCUSDT";
    i.tick_size = 0.1;
    i.lot_size = 0.001;
    return i;
}

// Bybit's USDC perpetual, used in the real messages of tests/data/bybit/.
Instrument btc_perp() {
    Instrument i = btc();
    i.symbol = "BTCPERP";
    return i;
}

// Reads a real message saved in tests/data/bybit/ (see the README there).
std::string real_message(const char* name) {
    std::ifstream file(std::string(PEEWEE_TEST_DATA_DIR) + "/bybit/" + name);
    CHECK(file.is_open());
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

}  // namespace

TEST(make_create_writes_price_with_tick_decimals) {
    const Instrument inst = btc();
    NewOrder o;
    o.px = inst.price(65000.3);
    o.qty = inst.qty(0.001);

    const std::string msg = bybit::make_create(o, inst, "linear", 1711001595207);

    CHECK(msg.find("\"price\":\"65000.3\"") != std::string::npos);
}

TEST(make_create_keeps_small_tick_decimals) {
    Instrument inst;
    inst.symbol = "PEPEUSDT";
    inst.tick_size = 0.0000001;
    inst.lot_size = 100;
    NewOrder o;
    o.px = inst.price(0.0000123);
    o.qty = inst.qty(1500);

    const std::string msg = bybit::make_create(o, inst, "linear", 1711001595207);

    CHECK(msg.find("\"price\":\"0.0000123\"") != std::string::npos);
}

TEST(parse_orderbook_reads_snapshot) {
    Instrument inst = btc();
    inst.id = 7;
    bybit::SymbolMap symbols{{"BTCUSDT", &inst}};

    const auto delta = bybit::parse_orderbook(
        R"({"topic":"orderbook.50.BTCUSDT","type":"snapshot","ts":1672304484978,
            "data":{"s":"BTCUSDT","b":[["16493.50","0.006"],["16493.00","0.100"]],
                    "a":[["16611.00","0.029"]],"u":18521288,"seq":7961638724},
            "cts":1672304484976})",
        symbols);

    CHECK(delta.has_value());
    CHECK_EQ(delta->inst, 7u);
    CHECK(delta->snapshot);
    CHECK_EQ(delta->seq, 18521288u);
    CHECK(delta->ts == std::chrono::milliseconds(1672304484978));
    CHECK_EQ(delta->bids.size(), 2u);
    CHECK_EQ(delta->bids[0].px, inst.price(16493.50));
    CHECK_EQ(delta->bids[0].qty, inst.qty(0.006));
    CHECK_EQ(delta->bids[1].px, inst.price(16493.00));
    CHECK_EQ(delta->asks.size(), 1u);
    CHECK_EQ(delta->asks[0].px, inst.price(16611.00));
    CHECK_EQ(delta->asks[0].qty, inst.qty(0.029));
}

TEST(parse_orderbook_reads_delta_with_removed_level) {
    Instrument inst = btc();
    bybit::SymbolMap symbols{{"BTCUSDT", &inst}};

    const auto delta = bybit::parse_orderbook(
        R"({"topic":"orderbook.50.BTCUSDT","type":"delta","ts":1672304484979,
            "data":{"s":"BTCUSDT","b":[["16493.50","0"]],"a":[],"u":18521289,"seq":7961638725},
            "cts":1672304484977})",
        symbols);

    CHECK(delta.has_value());
    CHECK(!delta->snapshot);
    CHECK_EQ(delta->bids.size(), 1u);
    CHECK(delta->bids[0].qty.is_zero());
    CHECK(delta->asks.empty());
}

TEST(parse_orderbook_treats_update_id_1_as_snapshot) {
    Instrument inst = btc();
    bybit::SymbolMap symbols{{"BTCUSDT", &inst}};

    const auto delta = bybit::parse_orderbook(
        R"({"topic":"orderbook.50.BTCUSDT","type":"delta","ts":1672304484979,
            "data":{"s":"BTCUSDT","b":[],"a":[],"u":1,"seq":7961638725},"cts":1672304484977})",
        symbols);

    CHECK(delta.has_value());
    CHECK(delta->snapshot);
}

TEST(parse_orderbook_rejects_unknown_symbol) {
    Instrument inst = btc();
    bybit::SymbolMap symbols{{"BTCUSDT", &inst}};

    const auto delta = bybit::parse_orderbook(
        R"({"topic":"orderbook.50.ETHUSDT","type":"snapshot","ts":1672304484978,
            "data":{"s":"ETHUSDT","b":[],"a":[],"u":5,"seq":1},"cts":1672304484976})",
        symbols);

    CHECK(!delta.has_value());
}

TEST(parse_orderbook_rejects_bad_price) {
    Instrument inst = btc();
    bybit::SymbolMap symbols{{"BTCUSDT", &inst}};

    const auto delta = bybit::parse_orderbook(
        R"({"topic":"orderbook.50.BTCUSDT","type":"snapshot","ts":1672304484978,
            "data":{"s":"BTCUSDT","b":[["abc","1"]],"a":[],"u":5,"seq":1},"cts":1672304484976})",
        symbols);

    CHECK(!delta.has_value());
}

TEST(make_create_puts_cid_and_req_in_req_id) {
    const Instrument inst = btc();
    NewOrder o;
    o.cid = 42;
    o.req = 7;
    o.px = inst.price(65000.3);
    o.qty = inst.qty(0.001);

    const std::string msg = bybit::make_create(o, inst, "linear", 1711001595207);

    CHECK(msg.find("\"reqId\":\"42-7\"") != std::string::npos);
}

TEST(parse_trade_response_ignores_success) {
    const auto event = bybit::parse_trade_response(
        R"({"reqId":"42-7","retCode":0,"retMsg":"OK","op":"order.create",
            "data":{"orderId":"a4c1718e-fe53-4659-a118-1f6ecce04ad9","orderLinkId":"42"},
            "retExtInfo":{},"header":{"Timenow":"1711001595209"},"connId":"abc"})");

    CHECK(!event.has_value());
}

TEST(parse_trade_response_rejects_create) {
    const auto event = bybit::parse_trade_response(
        R"({"reqId":"42-7","retCode":110007,"retMsg":"Available balance is insufficient",
            "op":"order.create","data":{},"retExtInfo":{},
            "header":{"Timenow":"1711001595209"},"connId":"abc"})");

    CHECK(event.has_value());
    const auto* reject = std::get_if<OrderReject>(&*event);
    CHECK(reject != nullptr);
    CHECK_EQ(reject->cid, 42u);
    CHECK_EQ(reject->req, 7u);
    CHECK(reject->request == RequestKind::New);
    CHECK(reject->reason == RejectReason::InsufficientBalance);
    CHECK_EQ(reject->text, "Available balance is insufficient");
    CHECK(reject->ts == std::chrono::milliseconds(1711001595209));
}

TEST(parse_trade_response_rejects_amend_of_missing_order) {
    const auto event = bybit::parse_trade_response(
        R"({"reqId":"42-8","retCode":110001,"retMsg":"Order does not exist",
            "op":"order.amend","data":{},"retExtInfo":{},"header":{},"connId":"abc"})");

    CHECK(event.has_value());
    const auto* reject = std::get_if<OrderReject>(&*event);
    CHECK(reject != nullptr);
    CHECK(reject->request == RequestKind::Amend);
    CHECK(reject->reason == RejectReason::OrderNotFound);
    CHECK(reject->ts == Timestamp{});
}

TEST(parse_trade_response_needs_our_req_id) {
    const auto event = bybit::parse_trade_response(
        R"({"retCode":10001,"retMsg":"Request parameter error","op":"order.create"})");

    CHECK(!event.has_value());
}

TEST(parse_order_topic_acks_new_order) {
    Instrument inst = btc();
    bybit::SymbolMap symbols{{"BTCUSDT", &inst}};

    const auto events = bybit::parse_order_topic(
        R"({"topic":"order","creationTime":1672364262474,
            "data":[{"orderId":"5cf98598","orderLinkId":"42","symbol":"BTCUSDT",
                     "price":"65000.3","qty":"0.001","orderStatus":"New",
                     "updatedTime":"1672364262457"}]})",
        symbols);

    CHECK_EQ(events.size(), 2u);
    const auto* ack = std::get_if<OrderAck>(&events[0]);
    CHECK(ack != nullptr);
    CHECK_EQ(ack->cid, 42u);
    CHECK_EQ(ack->venue_oid, "5cf98598");
    CHECK_EQ(ack->px, inst.price(65000.3));
    CHECK_EQ(ack->qty, inst.qty(0.001));
    CHECK(ack->ts == std::chrono::milliseconds(1672364262457));
    const auto* amend = std::get_if<AmendAck>(&events[1]);
    CHECK(amend != nullptr);
    CHECK_EQ(amend->px, inst.price(65000.3));
}

TEST(make_create_sends_market_as_limit_ioc) {
    const Instrument inst = btc();
    NewOrder o;
    o.type = OrderType::Market;
    o.side = Side::Buy;
    o.px = inst.price(66000.0);  // Worst price accepted.
    o.qty = inst.qty(0.001);

    const std::string msg = bybit::make_create(o, inst, "linear", 1711001595207);

    CHECK(msg.find(R"("orderType":"Limit")") != std::string::npos);
    CHECK(msg.find(R"("price":"66000.0")") != std::string::npos);
    CHECK(msg.find(R"("timeInForce":"IOC")") != std::string::npos);
    CHECK(msg.find("trigger") == std::string::npos);
}

TEST(make_create_adds_trigger_fields) {
    const Instrument inst = btc();
    NewOrder o;
    o.type = OrderType::StopMarket;
    o.side = Side::Sell;  // Stop-loss of a long: triggers when the price falls.
    o.px = inst.price(59000.0);
    o.trigger_px = inst.price(60000.0);
    o.qty = inst.qty(0.001);
    o.reduce_only = true;

    const std::string stop = bybit::make_create(o, inst, "linear", 1711001595207);
    CHECK(stop.find(R"("timeInForce":"IOC","triggerPrice":"60000.0","triggerDirection":2,)"
                    R"("triggerBy":"MarkPrice","orderLinkId")") != std::string::npos);
    CHECK(stop.find(R"("reduceOnly":true)") != std::string::npos);

    o.type = OrderType::TakeProfitLimit;  // Take-profit of a long: triggers when the price rises.
    o.trigger_px = inst.price(70000.0);
    o.px = inst.price(69990.0);
    const std::string tp = bybit::make_create(o, inst, "linear", 1711001595207);
    CHECK(tp.find(R"("timeInForce":"GTC","triggerPrice":"70000.0","triggerDirection":1,)") != std::string::npos);
}

TEST(make_create_sends_fok) {
    const Instrument inst = btc();
    NewOrder o;
    o.px = inst.price(65000.0);
    o.qty = inst.qty(0.001);
    o.tif = Tif::FOK;
    CHECK(bybit::make_create(o, inst, "linear", 1).find(R"("timeInForce":"FOK")") != std::string::npos);
}

TEST(parse_order_topic_acks_untriggered_order) {
    Instrument inst = btc();
    bybit::SymbolMap symbols{{"BTCUSDT", &inst}};

    const auto events = bybit::parse_order_topic(
        R"({"topic":"order","data":[{"orderId":"5cf98598","orderLinkId":"42","symbol":"BTCUSDT",
            "price":"59000","qty":"0.001","orderStatus":"Untriggered","rejectReason":"EC_NoError",
            "updatedTime":"1672364262457"}]})",
        symbols);

    CHECK_EQ(events.size(), 2u);  // OrderAck + AmendAck, like "New".
    CHECK(std::get_if<OrderAck>(&events[0]) != nullptr);
    CHECK(std::get_if<AmendAck>(&events[1]) != nullptr);
}

TEST(stop_order_live_sequence) {
    // Real BTCPERP stop from 2026-11-22: Untriggered, Triggered, execution, Filled.
    Instrument inst = btc();
    inst.symbol = "BTCPERP";
    bybit::SymbolMap symbols{{"BTCPERP", &inst}};
    constexpr ClientOrderId cid = 1795348330874002;

    auto events = bybit::parse_order_topic(real_message("order_untriggered.json"), symbols);
    CHECK_EQ(events.size(), 2u);  // Accepted, waiting for the trigger: OrderAck + AmendAck.
    CHECK_EQ(std::get<OrderAck>(events[0]).cid, cid);
    CHECK_EQ(std::get<OrderAck>(events[0]).px, inst.price(61210.4));

    CHECK(bybit::parse_order_topic(real_message("order_triggered.json"), symbols).empty());

    events = bybit::parse_execution_topic(real_message("execution_stop_fill.json"), symbols);
    CHECK_EQ(events.size(), 1u);
    const auto& fill = std::get<Fill>(events[0]);
    CHECK_EQ(fill.cid, cid);
    CHECK_EQ(fill.px, inst.price(60931.1));
    CHECK(!fill.maker);

    events = bybit::parse_order_topic(real_message("order_stop_filled.json"), symbols);
    CHECK_EQ(events.size(), 1u);
    CHECK(std::get<OrderClosed>(events[0]).reason == CloseReason::Filled);
}

TEST(stop_cancelled_and_ioc_without_match_live_messages) {
    Instrument inst = btc();
    inst.symbol = "BTCPERP";
    bybit::SymbolMap symbols{{"BTCPERP", &inst}};

    auto events = bybit::parse_order_topic(real_message("order_stop_deactivated.json"), symbols);
    CHECK_EQ(events.size(), 1u);
    CHECK(std::get<OrderClosed>(events[0]).reason == CloseReason::Canceled);

    // A market (Limit IOC) that found nothing within its price: canceled, not rejected.
    events = bybit::parse_order_topic(real_message("order_ioc_no_fill.json"), symbols);
    CHECK_EQ(events.size(), 1u);
    CHECK(std::get<OrderClosed>(events[0]).reason == CloseReason::Canceled);
}

TEST(parse_order_topic_partial_fill_acks_only_new) {
    Instrument inst = btc();
    bybit::SymbolMap symbols{{"BTCUSDT", &inst}};

    const auto events = bybit::parse_order_topic(
        R"({"topic":"order","data":[{"orderId":"5cf98598","orderLinkId":"42","symbol":"BTCUSDT",
            "price":"65000.3","qty":"0.002","orderStatus":"PartiallyFilled",
            "rejectReason":"EC_NoError","updatedTime":"1672364262457"}]})",
        symbols);

    CHECK_EQ(events.size(), 1u);
    CHECK(std::get_if<OrderAck>(&events[0]) != nullptr);
}

TEST(parse_order_topic_closes_filled_and_cancelled_orders) {
    Instrument inst = btc();
    bybit::SymbolMap symbols{{"BTCUSDT", &inst}};

    const auto events = bybit::parse_order_topic(
        R"({"topic":"order","data":[
            {"orderId":"a","orderLinkId":"42","symbol":"BTCUSDT","price":"65000.3","qty":"0.001",
             "orderStatus":"Filled","rejectReason":"EC_NoError","updatedTime":"1672364262457"},
            {"orderId":"b","orderLinkId":"43","symbol":"BTCUSDT","price":"65000.3","qty":"0.001",
             "orderStatus":"Cancelled","rejectReason":"EC_NoError","updatedTime":"1672364262458"}]})",
        symbols);

    CHECK_EQ(events.size(), 2u);
    const auto* filled = std::get_if<OrderClosed>(&events[0]);
    CHECK(filled != nullptr);
    CHECK_EQ(filled->cid, 42u);
    CHECK(filled->reason == CloseReason::Filled);
    const auto* cancelled = std::get_if<OrderClosed>(&events[1]);
    CHECK(cancelled != nullptr);
    CHECK_EQ(cancelled->cid, 43u);
    CHECK(cancelled->reason == CloseReason::Canceled);
}

TEST(parse_order_topic_skips_filled_copy_of_lost_cancel) {
    Instrument inst = btc();
    bybit::SymbolMap symbols{{"BTCUSDT", &inst}};

    const auto events = bybit::parse_order_topic(
        R"({"topic":"order","data":[{"orderId":"a","orderLinkId":"42","symbol":"BTCUSDT",
            "price":"65000.3","qty":"0.001","orderStatus":"Filled",
            "rejectReason":"EC_OrigClOrdIDDoesNotExist","updatedTime":"1672364262457"}]})",
        symbols);

    CHECK(events.empty());
}

TEST(parse_order_topic_rejects_crossing_post_only) {
    Instrument inst = btc();
    bybit::SymbolMap symbols{{"BTCUSDT", &inst}};

    const auto events = bybit::parse_order_topic(
        R"({"topic":"order","data":[{"orderId":"a","orderLinkId":"42","symbol":"BTCUSDT",
            "price":"65000.3","qty":"0.001","orderStatus":"Cancelled",
            "rejectReason":"EC_PostOnlyWillTakeLiquidity","updatedTime":"1672364262457"}]})",
        symbols);

    CHECK_EQ(events.size(), 1u);
    const auto* reject = std::get_if<OrderReject>(&events[0]);
    CHECK(reject != nullptr);
    CHECK_EQ(reject->cid, 42u);
    CHECK(reject->request == RequestKind::New);
    CHECK(reject->reason == RejectReason::PostOnlyWouldCross);
}

TEST(parse_order_topic_skips_foreign_orders) {
    Instrument inst = btc();
    bybit::SymbolMap symbols{{"BTCUSDT", &inst}};

    const auto events = bybit::parse_order_topic(
        R"({"topic":"order","data":[
            {"orderId":"a","orderLinkId":"","symbol":"BTCUSDT","price":"65000.3","qty":"0.001",
             "orderStatus":"New","updatedTime":"1672364262457"},
            {"orderId":"b","orderLinkId":"44","symbol":"ETHUSDT","price":"3000.1","qty":"0.1",
             "orderStatus":"New","updatedTime":"1672364262457"}]})",
        symbols);

    CHECK(events.empty());
}

TEST(parse_execution_topic_reads_trade_fills) {
    Instrument inst = btc();
    bybit::SymbolMap symbols{{"BTCUSDT", &inst}};

    const auto events = bybit::parse_execution_topic(
        R"({"id":"1","topic":"execution","creationTime":1672364174455,"data":[
            {"category":"linear","symbol":"BTCUSDT","execFee":"0.005","execId":"7e2ae69c",
             "execPrice":"65000.3","execQty":"0.001","execType":"Trade","leavesQty":"0",
             "orderId":"5cf98598","orderLinkId":"42","execTime":"1672364174443","isMaker":true},
            {"category":"linear","symbol":"BTCUSDT","execFee":"0.1","execId":"funding-1",
             "execPrice":"65000.3","execQty":"0.001","execType":"Funding","leavesQty":"0",
             "orderId":"","orderLinkId":"","execTime":"1672364174443","isMaker":false}]})",
        symbols);

    CHECK_EQ(events.size(), 1u);
    const auto* fill = std::get_if<Fill>(&events[0]);
    CHECK(fill != nullptr);
    CHECK_EQ(fill->cid, 42u);
    CHECK_EQ(fill->exec_id, "7e2ae69c");
    CHECK_EQ(fill->px, inst.price(65000.3));
    CHECK_EQ(fill->qty, inst.qty(0.001));
    CHECK(fill->maker);
    CHECK(fill->ts == std::chrono::milliseconds(1672364174443));
}

TEST(parse_envelope_reads_topic_and_op) {
    const auto book = bybit::parse_envelope(R"({"topic":"orderbook.50.BTCUSDT","type":"delta","data":{}})");
    CHECK_EQ(book.topic, "orderbook.50.BTCUSDT");

    const auto auth = bybit::parse_envelope(R"({"success":true,"ret_msg":"","op":"auth","conn_id":"x"})");
    CHECK(auth.topic.empty());
    CHECK_EQ(auth.op, "auth");
    CHECK(auth.ok);

    const auto trade_auth = bybit::parse_envelope(R"({"retCode":0,"retMsg":"OK","op":"auth","connId":"x"})");
    CHECK_EQ(trade_auth.op, "auth");
    CHECK(trade_auth.ok);

    const auto failed = bybit::parse_envelope(R"({"success":false,"ret_msg":"Invalid apikey","op":"auth"})");
    CHECK(!failed.ok);
    CHECK_EQ(failed.text, "Invalid apikey");
}

TEST(parse_order_status_reads_open_order) {
    Instrument inst = btc();
    bybit::SymbolMap symbols{{"BTCUSDT", &inst}};

    const auto status = bybit::parse_order_status(
        R"({"retCode":0,"retMsg":"OK","result":{"category":"linear","nextPageCursor":"","list":[
            {"orderId":"5cf98598","orderLinkId":"42","symbol":"BTCUSDT","price":"65000.3","qty":"0.002",
             "orderStatus":"PartiallyFilled","cumExecQty":"0.001","updatedTime":"1672364262457"}]},
            "retExtInfo":{},"time":1672364262474})",
        symbols, 42);

    CHECK(status.has_value());
    CHECK(status->found);
    CHECK(status->open);
    CHECK_EQ(status->px, inst.price(65000.3));
    CHECK_EQ(status->qty, inst.qty(0.002));
    CHECK_EQ(status->filled, inst.qty(0.001));
}

TEST(parse_order_status_empty_list_means_not_found) {
    Instrument inst = btc();
    bybit::SymbolMap symbols{{"BTCUSDT", &inst}};

    const auto status = bybit::parse_order_status(
        R"({"retCode":0,"retMsg":"OK","result":{"category":"linear","list":[]},"time":1})", symbols, 42);

    CHECK(status.has_value());
    CHECK_EQ(status->cid, 42u);
    CHECK(!status->found);
}

// --- Real messages (tests/data/bybit/) ---

TEST(real_login_and_subscribe_answers_are_ok) {
    const auto private_auth = bybit::parse_envelope(real_message("private_auth_ok.json"));
    CHECK_EQ(private_auth.op, "auth");
    CHECK(private_auth.ok);

    const auto trade_auth = bybit::parse_envelope(real_message("trade_auth_ok.json"));
    CHECK_EQ(trade_auth.op, "auth");
    CHECK(trade_auth.ok);

    const auto subscribe = bybit::parse_envelope(real_message("private_subscribe_ok.json"));
    CHECK_EQ(subscribe.op, "subscribe");
    CHECK(subscribe.ok);
}

TEST(real_pongs_are_control_messages) {
    for (const char* name : {"private_pong.json", "trade_pong.json"}) {
        const auto env = bybit::parse_envelope(real_message(name));
        CHECK(env.topic.empty());
        CHECK_EQ(env.op, "pong");
    }
}

TEST(real_order_new_acks_the_order) {
    Instrument inst = btc_perp();
    bybit::SymbolMap symbols{{"BTCPERP", &inst}};

    const auto msg = real_message("order_new.json");
    CHECK_EQ(bybit::parse_envelope(msg).topic, "order");
    const auto events = bybit::parse_order_topic(msg, symbols);

    CHECK_EQ(events.size(), 2u);
    const auto* ack = std::get_if<OrderAck>(&events[0]);
    CHECK(ack != nullptr);
    CHECK_EQ(ack->cid, 1u);
    CHECK_EQ(ack->venue_oid, "4ca1b1d4-9e11-4a9b-9102-1eab156add53");
    CHECK_EQ(ack->px, inst.price(60000.0));
    CHECK_EQ(ack->qty, inst.qty(0.001));
    CHECK(ack->ts == std::chrono::milliseconds(1794173338086));
    CHECK(std::get_if<AmendAck>(&events[1]) != nullptr);
}

TEST(real_order_cancelled_closes_the_order) {
    Instrument inst = btc_perp();
    bybit::SymbolMap symbols{{"BTCPERP", &inst}};

    const auto events = bybit::parse_order_topic(real_message("order_cancelled.json"), symbols);

    CHECK_EQ(events.size(), 1u);
    const auto* closed = std::get_if<OrderClosed>(&events[0]);
    CHECK(closed != nullptr);
    CHECK_EQ(closed->cid, 1u);
    CHECK(closed->reason == CloseReason::Canceled);
}

TEST(real_accepted_requests_produce_no_event) {
    for (const char* name : {"trade_create_ok.json", "trade_cancel_ok.json"}) {
        const auto msg = real_message(name);
        CHECK(bybit::parse_envelope(msg).ok);
        CHECK(!bybit::parse_trade_response(msg).has_value());
    }
}

TEST(real_rejected_create_reports_insufficient_balance) {
    const auto event = bybit::parse_trade_response(real_message("trade_create_rejected_balance.json"));

    CHECK(event.has_value());
    const auto* reject = std::get_if<OrderReject>(&*event);
    CHECK(reject != nullptr);
    CHECK_EQ(reject->cid, 1u);
    CHECK_EQ(reject->req, 1u);
    CHECK(reject->request == RequestKind::New);
    CHECK(reject->reason == RejectReason::InsufficientBalance);
    CHECK_EQ(reject->text, "ab not enough for new order");
    CHECK(reject->ts == std::chrono::milliseconds(1794172926741));
}

TEST(real_amend_is_acked_with_the_new_price) {
    Instrument inst = btc_perp();
    bybit::SymbolMap symbols{{"BTCPERP", &inst}};

    CHECK(!bybit::parse_trade_response(real_message("trade_amend_ok.json")).has_value());

    const auto events = bybit::parse_order_topic(real_message("order_amended.json"), symbols);
    CHECK_EQ(events.size(), 2u);
    const auto* amend = std::get_if<AmendAck>(&events[1]);
    CHECK(amend != nullptr);
    CHECK_EQ(amend->cid, 1794174593377000u);
    CHECK_EQ(amend->px, inst.price(75263.0));
    CHECK_EQ(amend->qty, inst.qty(0.001));
}

TEST(real_execution_is_a_maker_fill) {
    Instrument inst = btc_perp();
    bybit::SymbolMap symbols{{"BTCPERP", &inst}};

    const auto msg = real_message("execution_fill.json");
    CHECK_EQ(bybit::parse_envelope(msg).topic, "execution");
    const auto events = bybit::parse_execution_topic(msg, symbols);

    CHECK_EQ(events.size(), 1u);
    const auto* fill = std::get_if<Fill>(&events[0]);
    CHECK(fill != nullptr);
    CHECK_EQ(fill->cid, 1794174593377000u);
    CHECK_EQ(fill->exec_id, "d44b01d2-3492-517c-b6ae-453d77a77c3b");
    CHECK_EQ(fill->px, inst.price(75263.0));
    CHECK_EQ(fill->qty, inst.qty(0.001));
    CHECK(fill->maker);
    CHECK(fill->ts == std::chrono::milliseconds(1794174606722));
}

TEST(real_order_filled_closes_as_filled) {
    Instrument inst = btc_perp();
    bybit::SymbolMap symbols{{"BTCPERP", &inst}};

    const auto events = bybit::parse_order_topic(real_message("order_filled.json"), symbols);

    CHECK_EQ(events.size(), 1u);
    const auto* closed = std::get_if<OrderClosed>(&events[0]);
    CHECK(closed != nullptr);
    CHECK_EQ(closed->cid, 1794174593377000u);
    CHECK(closed->reason == CloseReason::Filled);
}

TEST(parse_instrument_info_reads_tick_and_lot) {
    const auto spec = bybit::parse_instrument_info(
        R"({"retCode":0,"retMsg":"OK","result":{"category":"linear","list":[
            {"symbol":"TRXPERP","contractType":"LinearPerpetual","status":"Trading",
             "priceFilter":{"minPrice":"0.0001","maxPrice":"199.9998","tickSize":"0.00010"},
             "lotSizeFilter":{"maxOrderQty":"1000000","minOrderQty":"1","qtyStep":"1"}}]},"time":1})");

    CHECK(spec.has_value());
    CHECK(spec->tick_size == 0.0001);
    CHECK(spec->lot_size == 1.0);
    CHECK(spec->min_qty == 1.0);
}

TEST(parse_instrument_info_unknown_symbol) {
    CHECK(!bybit::parse_instrument_info(R"({"retCode":10001,"retMsg":"params error: symbol invalid","result":{}})"));
    CHECK(!bybit::parse_instrument_info(R"({"retCode":0,"retMsg":"OK","result":{"list":[]}})"));
}

TEST(hmac_sha256_matches_rfc4231) {
    // RFC 4231, test case 2.
    CHECK_EQ(bybit::hmac_sha256_hex("Jefe", "what do ya want for nothing?"),
             "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_MAIN()
