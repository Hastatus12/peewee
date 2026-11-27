#include "check.hpp"
#include "lighter_codec.hpp"

#include <fstream>
#include <sstream>
#include <variant>

using namespace peewee;

namespace {

// Reads a real message saved in tests/data/lighter/ (see the README there).
std::string real_message(const char* name) {
    std::ifstream file(std::string(PEEWEE_TEST_DATA_DIR) + "/lighter/" + name);
    CHECK(file.is_open());
    std::stringstream text;
    text << file.rdbuf();
    std::string s = text.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// BTC perpetual on Lighter testnet: market 4096, 1 price decimal, 5 size decimals.
Instrument btc() {
    Instrument i;
    i.id = 7;
    i.symbol = "BTC";
    i.tick_size = 0.1;
    i.lot_size = 0.00001;
    return i;
}

constexpr int64_t kMarket = 4096;
constexpr int64_t kAccount = 12345;

// One Order JSON as documented (developers WebSocket reference), with the
// fields the codec reads.
std::string order_json(const char* status, const char* filled = "0.00000") {
    return std::string(R"({"order_index":562949953421312,"client_order_index":42,"order_id":"562949953421312",)"
                       R"("market_index":4096,"owner_account_index":12345,"initial_base_amount":"0.00020",)"
                       R"("price":"60000.5","nonce":7,"remaining_base_amount":"0.00020","is_ask":false,)"
                       R"("filled_base_amount":")") +
           filled + R"(","filled_quote_amount":"0.000000","side":"buy","type":"limit",)"
                    R"("time_in_force":"post-only","reduce_only":false,"trigger_price":"0.0","order_expiry":1800000000000,)"
                    R"("status":")" +
           status + R"(","timestamp":1795691754,"transaction_time":1795691754219534})";
}

std::string orders_message(const std::string& order) {
    return R"({"channel":"account_all_orders:12345","orders":{"4096":[)" + order +
           R"(]},"type":"update/account_all_orders"})";
}

}  // namespace

TEST(parse_scaled_is_exact) {
    CHECK_EQ(*lighter::parse_scaled("60593.9", 1), 605939);
    CHECK_EQ(*lighter::parse_scaled("0.42497", 5), 42497);
    CHECK_EQ(*lighter::parse_scaled("0.4", 5), 40000);
    CHECK_EQ(*lighter::parse_scaled("60000", 1), 600000);
    CHECK_EQ(*lighter::parse_scaled("0.00000", 5), 0);
    CHECK_EQ(*lighter::parse_scaled("1.2300", 2), 123);  // Trailing zeros beyond the decimals are fine.
    CHECK_EQ(*lighter::parse_scaled("-1.5", 1), -15);
    CHECK(!lighter::parse_scaled("1.234", 2));  // Finer than one step.
    CHECK(!lighter::parse_scaled("", 2));
    CHECK(!lighter::parse_scaled("1.2.3", 2));
    CHECK(!lighter::parse_scaled("abc", 2));
    CHECK(!lighter::parse_scaled(".", 2));
}

TEST(decimals_of_steps) {
    CHECK_EQ(lighter::decimals_of(0.1), 1);
    CHECK_EQ(lighter::decimals_of(0.00001), 5);
    CHECK_EQ(lighter::decimals_of(1.0), 0);
    CHECK_EQ(lighter::decimals_of(0.001), 3);
}

TEST(parse_order_book_reads_real_snapshot) {
    const Instrument inst = btc();
    const lighter::MarketMap markets{{kMarket, &inst}};

    const auto u = lighter::parse_order_book(real_message("order_book_snapshot.json"), markets);
    CHECK(u.has_value());
    CHECK(u->delta.snapshot);
    CHECK_EQ(u->delta.inst, inst.id);
    CHECK_EQ(u->delta.seq, 41726174u);
    CHECK_EQ(u->begin_nonce, 0u);
    CHECK(u->delta.ts == std::chrono::milliseconds(1795691756351));
    CHECK_EQ(u->delta.bids.size(), 45u);
    CHECK_EQ(u->delta.asks.size(), 53u);
    CHECK_EQ(u->delta.bids[0].px, inst.price(59538.6));
    CHECK_EQ(u->delta.bids[0].qty, inst.qty(15.44569));
    CHECK_EQ(u->delta.asks[0].px, inst.price(60313.7));
    CHECK_EQ(u->delta.asks[0].qty, inst.qty(0.02442));
}

TEST(parse_order_book_reads_real_deltas) {
    const Instrument inst = btc();
    const lighter::MarketMap markets{{kMarket, &inst}};

    const auto first = lighter::parse_order_book(real_message("order_book_update.json"), markets);
    CHECK(first.has_value());
    CHECK(!first->delta.snapshot);
    CHECK_EQ(first->begin_nonce, 41726174u);  // = the snapshot's nonce: continuous.
    CHECK_EQ(first->delta.seq, 41726191u);
    CHECK(first->delta.bids.empty());
    CHECK_EQ(first->delta.asks.size(), 2u);
    CHECK_EQ(first->delta.asks[0].px, inst.price(60614.0));
    CHECK(first->delta.asks[0].qty.is_zero());  // "0.00000": remove the level.

    const auto next = lighter::parse_order_book(real_message("order_book_update_next.json"), markets);
    CHECK(next.has_value());
    CHECK_EQ(next->begin_nonce, first->delta.seq);
}

TEST(parse_order_book_rejects_unknown_markets_and_other_channels) {
    const Instrument inst = btc();
    const lighter::MarketMap other{{1, &inst}};
    CHECK(!lighter::parse_order_book(real_message("order_book_update.json"), other));
    const lighter::MarketMap markets{{kMarket, &inst}};
    CHECK(!lighter::parse_order_book(real_message("sendtx_error.json"), markets));
    CHECK(!lighter::parse_order_book(R"({"session_id":"x","type":"connected"})", markets));
}

TEST(parse_account_orders_open_gives_ack) {
    const Instrument inst = btc();
    const lighter::MarketMap markets{{kMarket, &inst}};

    const auto events = lighter::parse_account_orders(orders_message(order_json("open")), markets);
    CHECK_EQ(events.size(), 2u);  // OrderAck + AmendAck: the OMS keeps the one it waits for.
    const auto* ack = std::get_if<OrderAck>(&events[0]);
    CHECK(ack != nullptr);
    CHECK_EQ(ack->cid, 42u);  // client_order_index: the venue maps it back to our cid.
    CHECK_EQ(ack->venue_oid, "562949953421312");
    CHECK_EQ(ack->px, inst.price(60000.5));
    CHECK_EQ(ack->qty, inst.qty(0.0002));
    CHECK(ack->ts == std::chrono::microseconds(1795691754219534));
    const auto* amend = std::get_if<AmendAck>(&events[1]);
    CHECK(amend != nullptr);
    CHECK_EQ(amend->px, ack->px);
}

TEST(parse_account_orders_maps_statuses) {
    const Instrument inst = btc();
    const lighter::MarketMap markets{{kMarket, &inst}};
    auto one = [&](const char* status) {
        auto events = lighter::parse_account_orders(orders_message(order_json(status)), markets);
        CHECK_EQ(events.size(), 1u);
        return events[0];
    };

    CHECK(std::get<OrderClosed>(one("filled")).reason == CloseReason::Filled);
    CHECK(std::get<OrderClosed>(one("canceled")).reason == CloseReason::Canceled);
    CHECK(std::get<OrderClosed>(one("canceled-too-much-slippage")).reason == CloseReason::Canceled);
    CHECK(std::get<OrderClosed>(one("canceled-not-enough-liquidity")).reason == CloseReason::Canceled);
    CHECK(std::get<OrderClosed>(one("canceled-expired")).reason == CloseReason::Expired);

    const auto reject = std::get<OrderReject>(one("canceled-post-only"));
    CHECK(reject.request == RequestKind::New);
    CHECK(reject.reason == RejectReason::PostOnlyWouldCross);

    // Waiting trigger orders are acknowledged; in-progress gives nothing yet.
    CHECK_EQ(lighter::parse_account_orders(orders_message(order_json("pending")), markets).size(), 2u);
    CHECK(lighter::parse_account_orders(orders_message(order_json("in-progress")), markets).empty());
}

TEST(parse_account_orders_open_size_follows_modify) {
    // Partly filled (0.0001), then modified to 0.0004: 0.0003 remaining. The
    // ack shows the current total, not initial_base_amount.
    const Instrument inst = btc();
    const lighter::MarketMap markets{{kMarket, &inst}};
    std::string order = order_json("open", "0.00010");
    order.replace(order.find(R"("remaining_base_amount":"0.00020")"), 33, R"("remaining_base_amount":"0.00030")");
    const auto events = lighter::parse_account_orders(orders_message(order), markets);
    CHECK_EQ(std::get<OrderAck>(events[0]).qty, inst.qty(0.0004));
    // Closed orders keep initial_base_amount (nothing remains).
    const auto query = lighter::parse_order_query(R"({"code":200,"orders":[)" + order_json("canceled", "0.00010") + "]}",
                                                  markets, 42);
    CHECK_EQ(query->qty, inst.qty(0.0002));
}

TEST(parse_account_orders_skips_unknown_markets_and_other_channels) {
    const Instrument inst = btc();
    const lighter::MarketMap other{{1, &inst}};
    CHECK(lighter::parse_account_orders(orders_message(order_json("open")), other).empty());
    const lighter::MarketMap markets{{kMarket, &inst}};
    CHECK(lighter::parse_account_orders(real_message("order_book_update.json"), markets).empty());
}

TEST(parse_account_trades_gives_our_sides) {
    const Instrument inst = btc();
    const lighter::MarketMap markets{{kMarket, &inst}};
    // We (12345) bought from a resting ask: taker on the bid side.
    const std::string msg =
        R"({"channel":"account_all_trades:12345","trades":{"4096":[{"trade_id":16164557907,)"
        R"("trade_id_str":"16164557907","tx_hash":"019f","type":"trade","market_id":4096,"size":"0.00020",)"
        R"("price":"60001.2","usd_amount":"12.000240","ask_id":281476612587355,"bid_id":562948334068259,)"
        R"("ask_client_id":363283,"bid_client_id":42,"ask_account_id":57890,"bid_account_id":12345,)"
        R"("is_maker_ask":true,"block_height":198321831,"timestamp":1773854156654}]},"type":"update/account_all_trades"})";

    const auto events = lighter::parse_account_trades(msg, markets, kAccount);
    CHECK_EQ(events.size(), 1u);
    const auto& fill = std::get<Fill>(events[0]);
    CHECK_EQ(fill.cid, 42u);
    CHECK_EQ(fill.exec_id, "16164557907b");
    CHECK_EQ(fill.px, inst.price(60001.2));
    CHECK_EQ(fill.qty, inst.qty(0.0002));
    CHECK(!fill.maker);
    CHECK(fill.ts == std::chrono::milliseconds(1773854156654));

    // Someone else's trade: nothing. Both sides ours (self-trade): two fills.
    CHECK(lighter::parse_account_trades(msg, markets, 999).empty());
    std::string self = msg;
    self.replace(self.find("57890"), 5, "12345");
    const auto both = lighter::parse_account_trades(self, markets, kAccount);
    CHECK_EQ(both.size(), 2u);
    CHECK(std::get<Fill>(both[0]).maker);  // The ask was the maker.
    CHECK_EQ(std::get<Fill>(both[0]).exec_id, "16164557907a");
}

TEST(parse_account_trades_skips_history_snapshot) {
    const Instrument inst = btc();
    const lighter::MarketMap markets{{kMarket, &inst}};
    CHECK(lighter::parse_account_trades(R"({"channel":"account_all_trades:12345","trades":[],"total_volume":0,)"
                                        R"("type":"subscribed/account_all_trades"})",
                                        markets, kAccount)
              .empty());
}

TEST(parse_error_reads_real_sendtx_error) {
    const auto e = lighter::parse_error(real_message("sendtx_error.json"));
    CHECK(e.has_value());
    CHECK_EQ(e->id, "77");
    CHECK_EQ(e->code, 21120);
    CHECK_EQ(e->text, "invalid signature");

    const auto sub = lighter::parse_error(real_message("subscribe_auth_error.json"));
    CHECK(sub.has_value());
    CHECK(sub->id.empty());
    CHECK_EQ(sub->code, 20013);

    CHECK(!lighter::parse_error(real_message("order_book_update.json")));
    CHECK(!lighter::parse_error(R"({"type":"pong"})"));
}

TEST(reject_reasons_from_codes) {
    CHECK(lighter::reject_reason_from_code(21739) == RejectReason::InsufficientBalance);
    CHECK(lighter::reject_reason_from_code(23000) == RejectReason::RateLimited);
    CHECK(lighter::reject_reason_from_code(21734) == RejectReason::InvalidPrice);
    CHECK(lighter::reject_reason_from_code(21701) == RejectReason::InvalidQty);
    CHECK(lighter::reject_reason_from_code(21715) == RejectReason::OrderNotFound);
    CHECK(lighter::reject_reason_from_code(21120) == RejectReason::Unknown);
    CHECK(lighter::is_nonce_error(21104));
    CHECK(!lighter::is_nonce_error(21120));
}

TEST(parse_order_query_finds_the_order) {
    const Instrument inst = btc();
    const lighter::MarketMap markets{{kMarket, &inst}};

    const auto open = lighter::parse_order_query(R"({"code":200,"orders":[)" + order_json("open") + "]}", markets, 42);
    CHECK(open.has_value());
    CHECK(open->found);
    CHECK(open->open);
    CHECK_EQ(open->px, inst.price(60000.5));
    CHECK_EQ(open->qty, inst.qty(0.0002));

    const auto filled =
        lighter::parse_order_query(R"({"code":200,"orders":[)" + order_json("filled", "0.00020") + "]}", markets, 42);
    CHECK(filled.has_value());
    CHECK(!filled->open);
    CHECK_EQ(filled->filled, inst.qty(0.0002));

    const auto none = lighter::parse_order_query(R"({"code":200,"orders":[]})", markets, 42);
    CHECK(none.has_value());
    CHECK(!none->found);

    // Not a list of orders (real answer without auth): ask again later.
    CHECK(!lighter::parse_order_query(
        R"({"code":20001,"message":"invalid param : auth query param and Authorization header are empty"})", markets,
        42));
}

TEST(parse_market_details_reads_real_answer) {
    const auto markets = lighter::parse_market_details(real_message("order_book_details.json"));
    CHECK(markets.has_value());
    CHECK_EQ(markets->size(), 3u);  // Perpetuals only: the spot markets are skipped.
    const auto& btc = markets->at("BTC");
    CHECK_EQ(btc.market_id, 4096);
    CHECK_EQ(btc.size_decimals, 5);
    CHECK_EQ(btc.price_decimals, 1);
    CHECK(btc.min_base_amount > 0.000199 && btc.min_base_amount < 0.000201);
    CHECK_EQ(markets->at("ETH").market_id, 4095);
}

TEST(parse_api_key_and_nonce_read_real_answers) {
    CHECK_EQ(*lighter::parse_api_key(real_message("apikeys.json")),
             "b65fbcd99061f422c3d9eb84f9fa3d43bec8b5a83de66ec4fca422ca1d356845d9254c8445d49097");
    CHECK(!lighter::parse_api_key(R"({"code":200,"api_keys":[]})"));
    CHECK_EQ(*lighter::parse_next_nonce(real_message("next_nonce.json")), 1);
    CHECK(!lighter::parse_next_nonce(R"({"code":21100,"message":"account not found"})"));
}

TEST(order_spec_maps_library_types) {
    auto spec = lighter::order_spec(OrderType::Limit, Tif::GTC);
    CHECK(spec && spec->type == 0 && spec->time_in_force == 1 && spec->expires);
    spec = lighter::order_spec(OrderType::Limit, Tif::PostOnly);
    CHECK(spec && spec->type == 0 && spec->time_in_force == 2 && spec->expires);
    spec = lighter::order_spec(OrderType::Limit, Tif::IOC);
    CHECK(spec && spec->type == 0 && spec->time_in_force == 0 && !spec->expires);
    CHECK(!lighter::order_spec(OrderType::Limit, Tif::FOK));
    spec = lighter::order_spec(OrderType::Market, Tif::GTC);  // tif ignored for market types.
    CHECK(spec && spec->type == 1 && spec->time_in_force == 0 && !spec->expires);
    spec = lighter::order_spec(OrderType::StopMarket, Tif::GTC);
    CHECK(spec && spec->type == 2 && spec->time_in_force == 0 && spec->expires);
    spec = lighter::order_spec(OrderType::StopLimit, Tif::GTC);
    CHECK(spec && spec->type == 3 && spec->time_in_force == 1 && spec->expires);
    spec = lighter::order_spec(OrderType::TakeProfitMarket, Tif::GTC);
    CHECK(spec && spec->type == 4 && spec->time_in_force == 0);
    spec = lighter::order_spec(OrderType::TakeProfitLimit, Tif::GTC);
    CHECK(spec && spec->type == 5 && spec->time_in_force == 1);
}

TEST(make_messages) {
    CHECK_EQ(lighter::make_subscribe("order_book/4096"), R"({"type":"subscribe","channel":"order_book/4096"})");
    CHECK_EQ(lighter::make_subscribe("account_all_orders/1", "tok"),
             R"({"type":"subscribe","channel":"account_all_orders/1","auth":"tok"})");
    CHECK_EQ(lighter::make_unsubscribe("order_book/1"), R"({"type":"unsubscribe","channel":"order_book/1"})");
    CHECK_EQ(lighter::make_send_tx(9, 15, R"({"Nonce":1})"),
             R"({"type":"jsonapi/sendtx","data":{"id":"9","tx_type":15,"tx_info":{"Nonce":1}}})");
}

TEST_MAIN()
