#pragma once
// Translation between Lighter JSON and normalized types. Pure functions only.
// Examples are real messages (tests/data/lighter/) or the WebSocket reference
// (https://docs.lighter.xyz/api/websocket).
//
// Lighter prices and sizes are decimal strings with the market's fixed
// decimals ("60593.9", "0.42497"). The instrument's tick is 10^-price_decimals
// and its lot 10^-size_decimals, so a Price in ticks IS Lighter's integer
// price, and a Qty in lots IS its base_amount.
//
// Order events carry Lighter's client_order_index in their `cid` field: the
// adapter maps it back to our ClientOrderId (ours can exceed Lighter's 2^48
// limit, see LighterVenue).

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "peewee/core/commands.hpp"
#include "peewee/core/events.hpp"
#include "peewee/core/instrument.hpp"

namespace peewee::lighter {

// The decimals of a tick or lot: 0.1 -> 1, 0.00001 -> 5, 1 -> 0.
int decimals_of(double step);

// An instrument with its decimals, computed once instead of on every message.
struct Market {
    Market(const Instrument* i)  // NOLINT: implicit, so {{id, &inst}} builds a MarketMap.
        : inst(i), px_dec(decimals_of(i->tick_size)), qty_dec(decimals_of(i->lot_size)) {}
    const Instrument* inst;
    int px_dec;
    int qty_dec;
};
using MarketMap = std::unordered_map<int64_t, Market>;  // market_id -> instrument

// Largest client_order_index Lighter accepts.
inline constexpr int64_t kMaxClientOrderIndex = (int64_t{1} << 48) - 1;

// --- From the exchange to the library ---

// Channel "order_book:{market}": first a full snapshot ("subscribed/order_book",
// every level of the book), then deltas ("update/order_book") every 50 ms:
// {"channel":"order_book:1","offset":10699383,"order_book":{"code":0,
//   "asks":[{"price":"60593.9","size":"0.00000"}],"bids":[],"offset":10699383,
//   "nonce":28933964502,"last_updated_at":1795691754219534,"begin_nonce":28933964433},
//  "timestamp":1795691754228,"type":"update/order_book"}
// A size of "0.00000" removes the level. BookDelta::seq is "nonce".
// A delta continues the previous message only if its begin_nonce equals the
// previous nonce: the caller checks that (see begin_nonce).
struct BookUpdate {
    BookDelta delta;
    uint64_t begin_nonce = 0;  // 0 for snapshots.
};
std::optional<BookUpdate> parse_order_book(std::string_view json, const MarketMap& markets);

// Channel "account_all_orders:{account}" (snapshot and updates):
// {"channel":"account_all_orders:1","orders":{"1":[Order, ...]},"type":"update/account_all_orders"}
// Order: {"order_index":..,"client_order_index":42,"market_index":1,"initial_base_amount":"0.00020",
//   "price":"60000.0","remaining_base_amount":"0.00020","filled_base_amount":"0.00000",
//   "is_ask":false,"status":"open","timestamp":1795691754,"transaction_time":1795691754219534,...}
// Status -> event:
//   open, pending (trigger order waiting)  -> OrderAck + AmendAck (the OMS keeps
//                                             the one matching its pending request)
//   filled                                 -> OrderClosed(Filled)
//   canceled-post-only                     -> OrderReject(New, PostOnlyWouldCross)
//   canceled-expired                       -> OrderClosed(Expired)
//   canceled, canceled-*                   -> OrderClosed(Canceled)
//   in-progress                            -> nothing (not yet in the book)
// Orders of unknown markets are skipped.
std::vector<OrderEvent> parse_account_orders(std::string_view json, const MarketMap& markets);

// Channel "account_all_trades:{account}" updates: one Fill per side of each
// trade that is ours (both sides for a self-trade).
// {"channel":"account_all_trades:1","trades":{"1":[Trade, ...]},"type":"update/account_all_trades"}
// Trade: {"trade_id":16164557907,"size":"0.1336","price":"2181.83","ask_client_id":363283,
//   "bid_client_id":23004521241,"ask_account_id":57890,"bid_account_id":317068,
//   "is_maker_ask":false,"market_id":0,"timestamp":1773854156654,...}
// exec_id = "<trade_id>a" / "<trade_id>b" (side). The first message
// ("subscribed/...") is old history: skipped.
std::vector<OrderEvent> parse_account_trades(std::string_view json, const MarketMap& markets, int64_t account);

// An error answer, e.g. to "jsonapi/sendtx" (real, testnet 2026-11-26):
// {"error":{"code":21120,"message":"invalid signature"},"id":"77"}
// id: the one we sent ("" if absent, e.g. errors of subscriptions).
struct ErrorMessage {
    std::string id;
    int code = 0;
    std::string text;
};
std::optional<ErrorMessage> parse_error(std::string_view json);

// Lighter's error codes (developers/reference/data-structures-constants-and-errors)
// as the library's reasons.
RejectReason reject_reason_from_code(int code);

// "invalid nonce" family: the nonce we signed with is not the one Lighter
// expects; re-read it from the API.
bool is_nonce_error(int code);

// REST GET /api/v1/accountOrders?account_index=..&client_order_indexes=<coi>:
// {"code":200,"orders":[Order, ...]}. The state of the order with this
// client_order_index (found = false if absent). nullopt if the answer is not
// a list of orders (HTTP error, auth error): the OMS then asks again.
std::optional<OrderStatus> parse_order_query(std::string_view json, const MarketMap& markets, int64_t coi);

// REST GET /api/v1/orderBookDetails: perpetual markets by symbol.
// {"code":200,"order_book_details":[{"symbol":"BTC","market_id":1,"market_type":"perp",
//   "min_base_amount":"0.00020","size_decimals":5,"price_decimals":1,...}],...}
struct MarketInfo {
    int64_t market_id = 0;
    int size_decimals = 0;
    int price_decimals = 0;
    double min_base_amount = 0.0;
};
std::optional<std::unordered_map<std::string, MarketInfo>> parse_market_details(std::string_view json);

// REST GET /api/v1/apikeys?account_index=..&api_key_index=..:
// {"code":200,"api_keys":[{"account_index":1,"api_key_index":0,"nonce":1,"public_key":"b65f..."}]}
// The public key (hex), or nullopt if the key is not registered.
std::optional<std::string> parse_api_key(std::string_view json);

// REST GET /api/v1/nextNonce?account_index=..&api_key_index=..: {"code":200,"nonce":1}
std::optional<int64_t> parse_next_nonce(std::string_view json);

// --- Formatting ---

// A decimal string as an integer number of steps of 10^-decimals, exactly
// (no double): parse_scaled("60593.9", 1) == 605939, parse_scaled("0.4", 5) == 40000.
// nullopt if it is not a number or has more significant decimals.
std::optional<int64_t> parse_scaled(std::string_view text, int decimals);

// --- From the library to the exchange ---

// How a library order is sent: Lighter's order type, time in force and
// whether it needs an expiry. nullopt if Lighter cannot do it (FOK).
//   Limit GTC       -> limit (0), good-till-time (1), expiry
//   Limit PostOnly  -> limit, post-only (2), expiry
//   Limit IOC       -> limit, IOC (0), no expiry
//   Market          -> market (1), IOC, no expiry (px = worst price)
//   StopMarket      -> stop-loss (2), IOC, expiry
//   StopLimit       -> stop-loss limit (3), good-till-time, expiry
//   TakeProfit*     -> take-profit (4) / take-profit limit (5), as Stop*
// Lighter has no GTC: good-till-time with the longest expiry allowed is used.
struct OrderSpec {
    uint8_t type = 0;
    uint8_t time_in_force = 0;
    bool expires = false;
};
std::optional<OrderSpec> order_spec(OrderType type, Tif tif);

// {"type":"subscribe","channel":"order_book/1"}
std::string make_subscribe(std::string_view channel);
// {"type":"subscribe","channel":"account_all_orders/1","auth":"<token>"}
std::string make_subscribe(std::string_view channel, std::string_view auth);
// {"type":"unsubscribe","channel":"order_book/1"}
std::string make_unsubscribe(std::string_view channel);

// {"type":"jsonapi/sendtx","data":{"id":"<id>","tx_type":14,"tx_info":<tx_info JSON>}}
// tx_info: a signed transaction from Signer, embedded as an object.
std::string make_send_tx(uint64_t id, uint8_t tx_type, std::string_view tx_info);

}  // namespace peewee::lighter
