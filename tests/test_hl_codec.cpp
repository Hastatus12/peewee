#include "check.hpp"
#include "hl_codec.hpp"

#include <fstream>
#include <sstream>
#include <variant>

using namespace peewee;

namespace {

// Reads a real message saved in tests/data/hyperliquid/ (see the README there).
std::string real_message(const char* name) {
    std::ifstream file(std::string(PEEWEE_TEST_DATA_DIR) + "/hyperliquid/" + name);
    CHECK(file.is_open());
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// BTC as configured in examples/hl_order_test.cpp, which produced those messages.
Instrument btc_live() {
    Instrument i;
    i.id = 4;
    i.symbol = "BTC";
    i.tick_size = 1.0;
    i.lot_size = 0.00001;
    return i;
}

constexpr ClientOrderId kLiveCid = 1794780113076;  // cloid 0x0000000000000000000001a1e13b28b4

}  // namespace

TEST(parse_order_updates_open_gives_ack) {
    const Instrument inst = btc_live();
    hyperliquid::CoinMap coins{{"BTC", &inst}};
    hyperliquid::LiveOids live;

    const auto events = hyperliquid::parse_order_updates(real_message("order_update_open.json"), coins, live);

    CHECK_EQ(events.size(), 2u);  // OrderAck + AmendAck: the OMS keeps the one it waits for.
    const auto* ack = std::get_if<OrderAck>(&events[0]);
    CHECK(ack != nullptr);
    CHECK_EQ(ack->cid, kLiveCid);
    CHECK_EQ(ack->venue_oid, "606372366681");
    CHECK_EQ(ack->px, inst.price(51989.0));
    CHECK_EQ(ack->qty, inst.qty(0.00024));
    CHECK(ack->ts == std::chrono::milliseconds(1794780113766));
    CHECK(std::holds_alternative<AmendAck>(events[1]));
}

TEST(parse_order_updates_canceled_gives_closed) {
    const Instrument inst = btc_live();
    hyperliquid::CoinMap coins{{"BTC", &inst}};
    hyperliquid::LiveOids live;

    const auto events = hyperliquid::parse_order_updates(real_message("order_update_canceled.json"), coins, live);

    CHECK_EQ(events.size(), 1u);
    const auto* closed = std::get_if<OrderClosed>(&events[0]);
    CHECK(closed != nullptr);
    CHECK_EQ(closed->cid, kLiveCid);
    CHECK(closed->reason == CloseReason::Canceled);
}

TEST(parse_order_updates_maps_statuses) {
    const Instrument inst = btc_live();
    hyperliquid::CoinMap coins{{"BTC", &inst}};
    hyperliquid::LiveOids live;
    auto with_status = [](const char* status) {
        return std::string(R"({"channel":"orderUpdates","data":[{"order":{"coin":"BTC","side":"B",)"
                           R"("limitPx":"51989.0","sz":"0.0","oid":1,"timestamp":1,"origSz":"0.00024",)"
                           R"("cloid":"0x0000000000000000000001a1e13b28b4"},"status":")") +
               status + R"(","statusTimestamp":2}]})";
    };

    auto events = hyperliquid::parse_order_updates(with_status("filled"), coins, live);
    CHECK_EQ(events.size(), 1u);
    CHECK(std::get<OrderClosed>(events[0]).reason == CloseReason::Filled);

    events = hyperliquid::parse_order_updates(with_status("marginCanceled"), coins, live);
    CHECK_EQ(events.size(), 1u);
    CHECK(std::get<OrderClosed>(events[0]).reason == CloseReason::Canceled);

    // Rejects come from the post response, not from here (no double reject).
    CHECK(hyperliquid::parse_order_updates(with_status("badAloPxRejected"), coins, live).empty());
    CHECK(hyperliquid::parse_order_updates(with_status("perpMarginRejected"), coins, live).empty());
}

TEST(trigger_order_live_sequence) {
    // Real stop-market from 2026-11-22: open (waiting), modify, cancel.
    const Instrument inst = btc_live();
    hyperliquid::CoinMap coins{{"BTC", &inst}};
    hyperliquid::LiveOids live;
    constexpr ClientOrderId cid = 1795346529826;  // cloid 0x...01a202fdfe22

    CHECK(!hyperliquid::parse_post_response(real_message("post_trigger_resting.json")).has_value());

    auto events = hyperliquid::parse_order_updates(real_message("order_update_trigger_open.json"), coins, live);
    CHECK_EQ(events.size(), 2u);
    CHECK_EQ(std::get<OrderAck>(events[0]).cid, cid);
    CHECK_EQ(std::get<OrderAck>(events[0]).px, inst.price(73719.0));  // The worst price, not the trigger.

    events = hyperliquid::parse_order_updates(real_message("order_update_trigger_modified.json"), coins, live);
    CHECK_EQ(events.size(), 2u);
    CHECK_EQ(std::get<OrderAck>(events[0]).px, inst.price(73720.0));
    for (const auto& e : events) CHECK(!std::holds_alternative<OrderClosed>(e));

    events = hyperliquid::parse_order_updates(real_message("order_update_trigger_canceled.json"), coins, live);
    CHECK_EQ(events.size(), 1u);
    CHECK(std::get<OrderClosed>(events[0]).reason == CloseReason::Canceled);
}

TEST(parse_order_updates_closes_on_a_newer_oid) {
    // If a triggered order fills under a new oid, the fill must close the order.
    const Instrument inst = btc_live();
    hyperliquid::CoinMap coins{{"BTC", &inst}};
    hyperliquid::LiveOids live{{1795346529826, 613131060699}};
    const auto events = hyperliquid::parse_order_updates(
        R"({"channel":"orderUpdates","data":[{"order":{"coin":"BTC","side":"B","limitPx":"73719.0","sz":"0.0",)"
        R"("oid":613131999999,"timestamp":1,"origSz":"0.00017","cloid":"0x0000000000000000000001a202fdfe22"},)"
        R"("status":"filled","statusTimestamp":2}]})",
        coins, live);
    CHECK_EQ(events.size(), 1u);
    CHECK(std::get<OrderClosed>(events[0]).reason == CloseReason::Filled);
    CHECK(live.empty());
}

TEST(triggered_stop_and_market_close_live_messages) {
    // Real run from 2026-11-22: the stop triggers and fills, then a market sell closes.
    const Instrument inst = btc_live();
    hyperliquid::CoinMap coins{{"BTC", &inst}};
    hyperliquid::LiveOids live{{1795346817858000, 613132896783}};  // The stop, already open.

    // "triggered" says nothing; "filled" (same oid) closes the order.
    auto events = hyperliquid::parse_order_updates(real_message("order_update_trigger_filled.json"), coins, live);
    CHECK_EQ(events.size(), 1u);
    CHECK_EQ(std::get<OrderClosed>(events[0]).cid, 1795346817858000u);
    CHECK(std::get<OrderClosed>(events[0]).reason == CloseReason::Filled);

    // The market close: accepted ("filled" status in the post), then open + filled at once.
    CHECK(!hyperliquid::parse_post_response(real_message("post_market_filled.json")).has_value());
    events = hyperliquid::parse_order_updates(real_message("order_update_market_filled.json"), coins, live);
    CHECK_EQ(events.size(), 3u);  // OrderAck + AmendAck + OrderClosed(Filled).
    CHECK(std::holds_alternative<OrderAck>(events[0]));
    CHECK(std::get<OrderClosed>(events[2]).reason == CloseReason::Filled);
    CHECK(live.empty());
}

TEST(parse_user_fills_reads_real_fill) {
    // The stop that triggered on 2026-11-22: bought 0.0002 BTC at 61071 as taker.
    const Instrument inst = btc_live();
    hyperliquid::CoinMap coins{{"BTC", &inst}};

    const auto events = hyperliquid::parse_user_fills(real_message("user_fills_stop_triggered.json"), coins);

    CHECK_EQ(events.size(), 1u);
    const auto* fill = std::get_if<Fill>(&events[0]);
    CHECK(fill != nullptr);
    CHECK_EQ(fill->cid, 1795346817858000u);       // From "cloid".
    CHECK_EQ(fill->exec_id, "698930659715340");   // "tid".
    CHECK_EQ(fill->px, inst.price(61071.0));
    CHECK_EQ(fill->qty, inst.qty(0.0002));
    CHECK(!fill->maker);                          // "crossed":true = taker.
    CHECK(fill->ts == std::chrono::milliseconds(1795346856581));
}

TEST(parse_user_fills_skips_snapshot_and_other_channels) {
    const Instrument inst = btc_live();
    hyperliquid::CoinMap coins{{"BTC", &inst}};
    CHECK(hyperliquid::parse_user_fills(real_message("user_fills_snapshot_empty.json"), coins).empty());
    CHECK(hyperliquid::parse_user_fills(real_message("order_update_open.json"), coins).empty());
}

TEST(parse_order_status_reads_real_answers) {
    const Instrument inst = btc_live();
    hyperliquid::CoinMap coins{{"BTC", &inst}};

    // The reduce-only market sell of 2026-11-22: fully filled.
    const auto filled = hyperliquid::parse_order_status(real_message("order_status_filled.json"), coins, 42);
    CHECK(filled.has_value());
    CHECK_EQ(filled->cid, 42u);
    CHECK(filled->found);
    CHECK(!filled->open);
    CHECK_EQ(filled->px, inst.price(60459.0));
    CHECK_EQ(filled->qty, inst.qty(0.0002));
    CHECK_EQ(filled->filled, inst.qty(0.0002));

    // The stop that triggered: also filled.
    const auto stop = hyperliquid::parse_order_status(real_message("order_status_stop_filled.json"), coins, 43);
    CHECK(stop.has_value() && stop->found && !stop->open);
    CHECK_EQ(stop->filled, inst.qty(0.0002));

    // A cloid Hyperliquid never saw.
    const auto unknown = hyperliquid::parse_order_status(real_message("order_status_unknown.json"), coins, 44);
    CHECK(unknown.has_value() && !unknown->found);

    CHECK(!hyperliquid::parse_order_status("Failed to parse the request body as JSON", coins, 45).has_value());
}

TEST(make_order_status_request_uses_cloid) {
    CHECK_EQ(hyperliquid::make_order_status_request("0x1111111111111111111111111111111111111111",
                                                    "0x00000000000000000000000000000001"),
             R"({"type":"orderStatus","user":"0x1111111111111111111111111111111111111111",)"
             R"("oid":"0x00000000000000000000000000000001"})");
}

TEST(parse_mid_reads_one_coin) {
    const char* json = R"({"#114910":"0.5","BTC":"61000.5","HYPE":"64.25"})";
    CHECK(hyperliquid::parse_mid(json, "HYPE").has_value());
    CHECK_EQ(*hyperliquid::parse_mid(json, "HYPE"), 64.25);
    CHECK_EQ(*hyperliquid::parse_mid(json, "BTC"), 61000.5);
    CHECK(!hyperliquid::parse_mid(json, "NOPE").has_value());
    CHECK(!hyperliquid::parse_mid("not json", "BTC").has_value());
}

TEST(tick_for_price_follows_hyperliquid_rules) {
    using hyperliquid::tick_for_price;
    CHECK_EQ(tick_for_price(61000.0, 5), 1.0);     // BTC.
    CHECK_EQ(tick_for_price(120000.0, 5), 1.0);    // Integers are always fine.
    CHECK(std::abs(tick_for_price(3000.0, 4) - 0.1) < 1e-12);      // ETH.
    CHECK(std::abs(tick_for_price(0.1234, 0) - 0.00001) < 1e-12);  // 5 significant digits.
    CHECK(std::abs(tick_for_price(0.001234, 0) - 0.000001) < 1e-15);  // Capped by 6 - szDecimals.
    // Every price on the tick grid near the reference is valid.
    CHECK(hyperliquid::is_valid_price(hyperliquid::format_wire(30005, 0.1), 4));
}

TEST(market_without_match_live_messages) {
    // Real market buy from 2026-11-22 with a worst price below the market.
    const auto err = hyperliquid::parse_post_response(real_message("post_market_no_match.json"));
    CHECK(err.has_value());
    CHECK_EQ(err->id, 4u);
    CHECK(hyperliquid::is_ioc_without_match(err->text));
    CHECK(!hyperliquid::is_ioc_without_match("Order must have minimum value of $10."));

    // The same news in orderUpdates is skipped: only one event reaches the algorithm.
    const Instrument inst = btc_live();
    hyperliquid::CoinMap coins{{"BTC", &inst}};
    hyperliquid::LiveOids live;
    CHECK(hyperliquid::parse_order_updates(real_message("order_update_ioc_cancel_rejected.json"), coins, live)
              .empty());
}

TEST(parse_order_updates_skips_orders_without_our_cloid) {
    const Instrument inst = btc_live();
    hyperliquid::CoinMap coins{{"BTC", &inst}};
    hyperliquid::LiveOids live;
    // Placed from the website: no "cloid" field at all.
    CHECK(hyperliquid::parse_order_updates(
              R"({"channel":"orderUpdates","data":[{"order":{"coin":"BTC","side":"B","limitPx":"51989.0",)"
              R"("sz":"0.00024","oid":1,"timestamp":1,"origSz":"0.00024"},"status":"open","statusTimestamp":2}]})",
              coins, live)
              .empty());
    // Not a book or order channel.
    CHECK(hyperliquid::parse_order_updates(real_message("post_cancel_success.json"), coins, live).empty());
}

TEST(parse_order_updates_modify_does_not_close_the_order) {
    // Real sequence from 2026-11-17: open, modify (new oid + old oid canceled), cancel.
    const Instrument inst = btc_live();
    hyperliquid::CoinMap coins{{"BTC", &inst}};
    hyperliquid::LiveOids live;
    constexpr ClientOrderId cid = 1794952071372;  // cloid 0x...01a1eb7b08cc

    auto events = hyperliquid::parse_order_updates(real_message("order_update_open_before_modify.json"), coins, live);
    CHECK_EQ(events.size(), 2u);
    CHECK_EQ(live.at(cid), 608577962115u);

    // The modify: only the ack for the new price, NO OrderClosed for the old copy.
    events = hyperliquid::parse_order_updates(real_message("order_update_modified.json"), coins, live);
    CHECK_EQ(events.size(), 2u);
    const auto* ack = std::get_if<OrderAck>(&events[0]);
    CHECK(ack != nullptr);
    CHECK_EQ(ack->px, inst.price(51458.0));
    CHECK_EQ(ack->venue_oid, "608578010231");
    CHECK(std::holds_alternative<AmendAck>(events[1]));
    CHECK_EQ(live.at(cid), 608578010231u);

    // The real cancel, of the current oid: now the order is closed.
    events = hyperliquid::parse_order_updates(real_message("order_update_canceled_after_modify.json"), coins, live);
    CHECK_EQ(events.size(), 1u);
    CHECK(std::get<OrderClosed>(events[0]).reason == CloseReason::Canceled);
    CHECK(live.empty());
}

TEST(parse_order_updates_modify_works_in_any_order) {
    // Same modify message, but with the old copy's cancel listed first.
    const Instrument inst = btc_live();
    hyperliquid::CoinMap coins{{"BTC", &inst}};
    hyperliquid::LiveOids live{{1794952071372, 608577962115}};
    const auto events = hyperliquid::parse_order_updates(
        R"({"channel":"orderUpdates","data":[)"
        R"({"order":{"coin":"BTC","side":"B","limitPx":"51459.0","sz":"0.00024","oid":608577962115,"timestamp":1,)"
        R"("origSz":"0.00024","cloid":"0x0000000000000000000001a1eb7b08cc"},"status":"canceled","statusTimestamp":2},)"
        R"({"order":{"coin":"BTC","side":"B","limitPx":"51458.0","sz":"0.00024","oid":608578010231,"timestamp":2,)"
        R"("origSz":"0.00024","cloid":"0x0000000000000000000001a1eb7b08cc"},"status":"open","statusTimestamp":2}]})",
        coins, live);
    CHECK_EQ(events.size(), 2u);
    for (const auto& e : events) CHECK(!std::holds_alternative<OrderClosed>(e));
}

TEST(make_subscribe_l2book_for_btc) {
    CHECK_EQ(hyperliquid::make_subscribe_l2book("BTC"),
             R"({"method":"subscribe","subscription":{"type":"l2Book","coin":"BTC","fast":true}})");
}

TEST(make_order_json_for_post_only_buy) {
    // Same order as the one signed in test_hl_signer.cpp (order_action).
    CHECK_EQ(hyperliquid::make_order_json(0, true, "65000", "0.001", false, Tif::PostOnly,
                                          "0x00000000000000000000000000000001"),
             R"({"type":"order","orders":[{"a":0,"b":true,"p":"65000","s":"0.001","r":false,)"
             R"("t":{"limit":{"tif":"Alo"}},"c":"0x00000000000000000000000000000001"}],"grouping":"na"})");
}

TEST(make_order_msgpack_matches_python_sdk) {
    // Bytes of msgpack.packb() for the same order, from hyperliquid-python-sdk
    // (also checked in test_hl_signer.cpp).
    const std::string bytes = hyperliquid::make_order_msgpack(0, true, "65000", "0.001", false, Tif::PostOnly,
                                                              "0x00000000000000000000000000000001");
    std::string hex;
    for (unsigned char c : bytes) {
        static constexpr char digits[] = "0123456789abcdef";
        hex += digits[c >> 4];
        hex += digits[c & 0x0f];
    }
    CHECK_EQ(hex,
             "83a474797065a56f72646572a66f72646572739187a16100a162c3a170a53635303030a173a5302e303031a172c2a17481a56c"
             "696d697481a3746966a3416c6fa163d92230783030303030303030303030303030303030303030303030303030303030303031a8"
             "67726f7570696e67a26e61");
}

namespace {

std::string to_hex(const std::string& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (unsigned char c : bytes) {
        hex += digits[c >> 4];
        hex += digits[c & 0x0f];
    }
    return hex;
}

}  // namespace

TEST(modify_and_cancel_msgpack_match_python_sdk) {
    // Same actions as in test_hl_signer.cpp; bytes from msgpack.packb().
    const std::string cloid = hyperliquid::make_cloid(1);
    CHECK_EQ(cloid, "0x00000000000000000000000000000001");

    const auto modify =
        hyperliquid::make_modify_action(3, false, "0.12345", "1000", false, hyperliquid::OrderKind{}, cloid);
    CHECK_EQ(to_hex(modify.msgpack),
             "83a474797065a66d6f64696679a36f6964d92230783030303030303030303030303030303030303030303030303030303030303031"
             "a56f7264657287a16103a162c2a170a7302e3132333435a173a431303030a172c2a17481a56c696d697481a3746966a3477463a1"
             "63d92230783030303030303030303030303030303030303030303030303030303030303031");
    CHECK_EQ(modify.json,
             R"({"type":"modify","oid":"0x00000000000000000000000000000001","order":{"a":3,"b":false,"p":"0.12345",)"
             R"("s":"1000","r":false,"t":{"limit":{"tif":"Gtc"}},"c":"0x00000000000000000000000000000001"}})");

    const auto cancel = hyperliquid::make_cancel_action(159, cloid);
    CHECK_EQ(to_hex(cancel.msgpack),
             "82a474797065ad63616e63656c4279436c6f6964a763616e63656c739182a56173736574cc9fa5636c6f6964"
             "d92230783030303030303030303030303030303030303030303030303030303030303031");
    CHECK_EQ(cancel.json,
             R"({"type":"cancelByCloid","cancels":[{"asset":159,"cloid":"0x00000000000000000000000000000001"}]})");
}

TEST(make_cloid_uses_last_16_hex_digits) {
    CHECK_EQ(hyperliquid::make_cloid(0x1794611089218000ULL), "0x00000000000000001794611089218000");
}

TEST(make_post_wraps_signed_action) {
    CHECK_EQ(hyperliquid::make_post(7, R"({"type":"noop"})", 1700000000000, "0xaa", "0xbb", 28),
             R"({"method":"post","id":7,"request":{"type":"action","payload":{"action":{"type":"noop"},)"
             R"("nonce":1700000000000,"signature":{"r":"0xaa","s":"0xbb","v":28},"vaultAddress":null}}})");
}

TEST(make_subscribe_user_for_order_updates) {
    CHECK_EQ(hyperliquid::make_subscribe_user("orderUpdates", "0xabc"),
             R"({"method":"subscribe","subscription":{"type":"orderUpdates","user":"0xabc"}})");
}

TEST(format_wire_strips_trailing_zeros) {
    CHECK_EQ(hyperliquid::format_wire(650003, 0.1), "65000.3");
    CHECK_EQ(hyperliquid::format_wire(650000, 0.1), "65000");
    CHECK_EQ(hyperliquid::format_wire(68000, 1.0), "68000");
    CHECK_EQ(hyperliquid::format_wire(100, 0.00001), "0.001");
    CHECK_EQ(hyperliquid::format_wire(12345, 0.00001), "0.12345");
    CHECK_EQ(hyperliquid::format_wire(1000, 1.0), "1000");
}

TEST(is_valid_price_follows_hyperliquid_rules) {
    CHECK(hyperliquid::is_valid_price("68000", 5));     // Integer: always fine.
    CHECK(hyperliquid::is_valid_price("123456", 5));    // Even with 6 digits.
    CHECK(hyperliquid::is_valid_price("6800.5", 5));    // 5 significant, 1 decimal.
    CHECK(!hyperliquid::is_valid_price("68000.5", 5));  // 6 significant digits.
    CHECK(!hyperliquid::is_valid_price("1.25", 5));     // BTC: at most 6 - 5 = 1 decimal.
    CHECK(hyperliquid::is_valid_price("0.12345", 0));   // Leading zeros do not count.
    CHECK(!hyperliquid::is_valid_price("0.123456", 0)); // 6 significant digits.
}

TEST(parse_meta_reads_asset_indexes) {
    const auto meta = hyperliquid::parse_meta(
        R"({"universe":[{"szDecimals":5,"name":"BTC","maxLeverage":40},)"
        R"({"szDecimals":4,"name":"ETH","maxLeverage":25},{"szDecimals":1,"name":"MATIC","isDelisted":true}]})");
    CHECK(meta.has_value());
    CHECK_EQ(meta->at("BTC").index, 0u);
    CHECK_EQ(meta->at("BTC").sz_decimals, 5);
    CHECK_EQ(meta->at("ETH").index, 1u);
    CHECK_EQ(meta->at("MATIC").index, 2u);
}

TEST(parse_agent_owner_reads_main_account) {
    CHECK_EQ(hyperliquid::parse_agent_owner(R"({"role":"agent","data":{"user":"0x1111"}})").value_or(""), "0x1111");
    CHECK(!hyperliquid::parse_agent_owner(R"({"role":"user"})"));
    CHECK(!hyperliquid::parse_agent_owner(R"({"role":"missing"})"));
}

TEST(make_order_json_for_reduce_only_sell) {
    CHECK_EQ(hyperliquid::make_order_json(159, false, "0.12345", "1000", true, Tif::IOC,
                                          "0x0000000000000000000000000000abcd"),
             R"({"type":"order","orders":[{"a":159,"b":false,"p":"0.12345","s":"1000","r":true,)"
             R"("t":{"limit":{"tif":"Ioc"}},"c":"0x0000000000000000000000000000abcd"}],"grouping":"na"})");
}

namespace {

Instrument btc() {
    Instrument i;
    i.id = 3;
    i.symbol = "BTC";
    i.tick_size = 1.0;
    i.lot_size = 0.00001;
    return i;
}

}  // namespace

TEST(parse_l2book_reads_full_book) {
    const Instrument inst = btc();
    hyperliquid::CoinMap coins{{"BTC", &inst}};

    const auto delta = hyperliquid::parse_l2book(
        R"({"channel":"l2Book","data":{"coin":"BTC","time":1700000000000,"levels":[
              [{"px":"65000.0","sz":"1.2","n":3},{"px":"64999.0","sz":"0.4","n":1}],
              [{"px":"65001.0","sz":"0.5","n":1}]]}})",
        coins);

    CHECK(delta.has_value());
    CHECK_EQ(delta->inst, 3u);
    CHECK(delta->snapshot);
    CHECK_EQ(delta->seq, 1700000000000u);
    CHECK(delta->ts == std::chrono::milliseconds(1700000000000));
    CHECK_EQ(delta->bids.size(), 2u);
    CHECK_EQ(delta->bids[0].px, inst.price(65000.0));
    CHECK_EQ(delta->bids[0].qty, inst.qty(1.2));
    CHECK_EQ(delta->bids[1].px, inst.price(64999.0));
    CHECK_EQ(delta->asks.size(), 1u);
    CHECK_EQ(delta->asks[0].px, inst.price(65001.0));
    CHECK_EQ(delta->asks[0].qty, inst.qty(0.5));
}

TEST(parse_l2book_ignores_subscription_response) {
    const Instrument inst = btc();
    hyperliquid::CoinMap coins{{"BTC", &inst}};
    CHECK(!hyperliquid::parse_l2book(
        R"({"channel":"subscriptionResponse","data":{"method":"subscribe",
            "subscription":{"type":"l2Book","coin":"BTC"}}})",
        coins));
}

TEST(parse_l2book_rejects_unknown_coin) {
    const Instrument inst = btc();
    hyperliquid::CoinMap coins{{"BTC", &inst}};
    CHECK(!hyperliquid::parse_l2book(
        R"({"channel":"l2Book","data":{"coin":"ETH","time":1,"levels":[[],[]]}})", coins));
}

TEST(parse_post_response_success_gives_nothing) {
    // Messaggi veri della tua prova: tutto ok, quindi nessun errore.
    CHECK(!hyperliquid::parse_post_response(real_message("post_order_resting.json")).has_value());
    CHECK(!hyperliquid::parse_post_response(real_message("post_cancel_success.json")).has_value());
    CHECK(!hyperliquid::parse_post_response(real_message("post_modify_ok.json")).has_value());
}

TEST(parse_post_response_reads_order_error) {
    const auto err = hyperliquid::parse_post_response(
        R"({"channel":"post","data":{"id":7,"response":{"type":"action","payload":{"status":"ok","response":{"type":"order","data":{"statuses":[{"error":"Order must have minimum value of $10."}]}}}}}})");
    CHECK(err.has_value());
    CHECK_EQ(err->id, 7u);
    CHECK_EQ(err->text, "Order must have minimum value of $10.");
}

TEST(parse_post_response_reads_cancel_and_action_errors) {
    const auto cancel = hyperliquid::parse_post_response(
        R"({"channel":"post","data":{"id":3,"response":{"type":"action","payload":{"status":"ok","response":{"type":"cancel","data":{"statuses":[{"error":"Order was never placed, already canceled, or filled."}]}}}}}})");
    CHECK(cancel.has_value());
    CHECK_EQ(cancel->id, 3u);
    CHECK_EQ(cancel->text, "Order was never placed, already canceled, or filled.");

    const auto action = hyperliquid::parse_post_response(
        R"({"channel":"post","data":{"id":5,"response":{"type":"action","payload":{"status":"err","response":"User or API Wallet does not exist."}}}})");
    CHECK(action.has_value());
    CHECK_EQ(action->id, 5u);
    CHECK_EQ(action->text, "User or API Wallet does not exist.");

    const auto request = hyperliquid::parse_post_response(
        R"({"channel":"post","data":{"id":6,"response":{"type":"error","payload":"Invalid request"}}})");
    CHECK(request.has_value());
    CHECK_EQ(request->id, 6u);
}

TEST(order_action_for_limit_matches_make_order_json_and_msgpack) {
    hyperliquid::OrderKind kind;
    kind.tif = Tif::PostOnly;
    const auto action = hyperliquid::make_order_action(0, true, "65000", "0.001", false, kind,
                                                       "0x00000000000000000000000000000001");
    CHECK_EQ(action.json, hyperliquid::make_order_json(0, true, "65000", "0.001", false, Tif::PostOnly,
                                                       "0x00000000000000000000000000000001"));
    CHECK_EQ(action.msgpack, hyperliquid::make_order_msgpack(0, true, "65000", "0.001", false, Tif::PostOnly,
                                                             "0x00000000000000000000000000000001"));
}

TEST(order_kind_maps_library_types) {
    using hyperliquid::order_kind;
    const auto market = order_kind(OrderType::Market, Tif::GTC, "");
    CHECK(market.has_value() && !market->trigger && market->tif == Tif::IOC);

    const auto stop = order_kind(OrderType::StopMarket, Tif::GTC, "61000");
    CHECK(stop.has_value() && stop->trigger && stop->is_market && !stop->take_profit);
    CHECK_EQ(stop->trigger_px, "61000");

    const auto tp = order_kind(OrderType::TakeProfitLimit, Tif::GTC, "70000");
    CHECK(tp.has_value() && tp->trigger && !tp->is_market && tp->take_profit);

    CHECK(!order_kind(OrderType::Limit, Tif::FOK, "").has_value());  // No FOK on Hyperliquid.
}

TEST(trigger_orders_match_python_sdk) {
    // Bytes of msgpack.packb() of the action built by hyperliquid-python-sdk's
    // order_request_to_order_wire + order_wires_to_order_action.
    const std::string cloid = "0x00000000000000000000000000000001";
    const auto stop = hyperliquid::make_order_action(
        0, false, "60000", "0.001", true, *hyperliquid::order_kind(OrderType::StopMarket, Tif::GTC, "61000"), cloid);
    CHECK_EQ(stop.json, R"({"type":"order","orders":[{"a":0,"b":false,"p":"60000","s":"0.001","r":true,)"
                        R"("t":{"trigger":{"isMarket":true,"triggerPx":"61000","tpsl":"sl"}},)"
                        R"("c":"0x00000000000000000000000000000001"}],"grouping":"na"})");
    CHECK_EQ(to_hex(stop.msgpack),
             "83a474797065a56f72646572a66f72646572739187a16100a162c2a170a53630303030a173a5302e303031a172c3a17481a7"
             "7472696767657283a869734d61726b6574c3a9747269676765725078a53631303030a47470736ca2736ca163d92230783030"
             "303030303030303030303030303030303030303030303030303030303031a867726f7570696e67a26e61");

    const auto tp = hyperliquid::make_order_action(
        0, false, "69990", "0.001", true, *hyperliquid::order_kind(OrderType::TakeProfitLimit, Tif::GTC, "70000"),
        cloid);
    CHECK_EQ(to_hex(tp.msgpack),
             "83a474797065a56f72646572a66f72646572739187a16100a162c2a170a53639393930a173a5302e303031a172c3a17481a7"
             "7472696767657283a869734d61726b6574c2a9747269676765725078a53730303030a47470736ca27470a163d92230783030"
             "303030303030303030303030303030303030303030303030303030303031a867726f7570696e67a26e61");
}

TEST(reject_reason_from_text_maps_known_errors) {
    using hyperliquid::reject_reason_from_text;
    CHECK(reject_reason_from_text("Post only order would have immediately matched, bbo was 1.0@2.0") ==
          RejectReason::PostOnlyWouldCross);
    CHECK(reject_reason_from_text("Order must have minimum value of $10.") == RejectReason::InvalidQty);
    CHECK(reject_reason_from_text("Insufficient margin to place order. asset=0") ==
          RejectReason::InsufficientBalance);
    CHECK(reject_reason_from_text("Order was never placed, already canceled, or filled.") ==
          RejectReason::OrderNotFound);
    CHECK(reject_reason_from_text("Price must be divisible by tick size. asset=0") == RejectReason::InvalidPrice);
    CHECK(reject_reason_from_text("something new") == RejectReason::Unknown);
}

TEST(parse_post_response_ignores_other_channels) {
    CHECK(!hyperliquid::parse_post_response(R"({"channel":"pong"})").has_value());
}

TEST_MAIN()