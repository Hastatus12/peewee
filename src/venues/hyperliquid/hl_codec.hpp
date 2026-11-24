#pragma once
// Translation between Hyperliquid JSON and normalized types. Pure functions only.

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "peewee/core/commands.hpp"
#include "peewee/core/events.hpp"
#include "peewee/core/instrument.hpp"

namespace peewee::hyperliquid {

using CoinMap = std::unordered_map<std::string, const Instrument*>;

// --- From the exchange to the library ---

// "l2Book" channel: Hyperliquid always sends the FULL book (snapshot), not deltas.
// {"channel":"l2Book","data":{"coin":"BTC","time":1700000000000,
//   "levels":[[{"px":"65000.0","sz":"1.2","n":3}, ...],   <- bids
//             [{"px":"65001.0","sz":"0.5","n":1}, ...]]}} <- asks
std::optional<BookDelta> parse_l2book(std::string_view json, const CoinMap& coins);

// Private channel "orderUpdates":
// {"channel":"orderUpdates","data":[{"order":{"coin":"BTC","side":"B","limitPx":"51989.0",
//   "sz":"0.00024","oid":606372366681,"timestamp":...,"origSz":"0.00024","cloid":"0x..."},
//   "status":"open","statusTimestamp":...}]}
// open -> OrderAck (+ AmendAck), filled -> OrderClosed(Filled),
// canceled / *Canceled -> OrderClosed(Canceled). *Rejected is skipped: the
// post response reports the same error (see parse_post_response).
// Trigger orders waiting for their trigger are "open" too (seen live).
// Orders without one of our cloids are skipped.
//
// live_oids: the current oid of each of our open orders, kept by the caller
// between calls. Needed because a modify creates a NEW oid and cancels the
// old one: that cancel must not close the order.
using LiveOids = std::unordered_map<ClientOrderId, uint64_t>;
std::vector<OrderEvent> parse_order_updates(std::string_view json, const CoinMap& coins, LiveOids& live_oids);

// Private channel "userFills": one Fill per execution of our orders.
// {"channel":"userFills","data":{"user":"0x...","fills":[{"coin":"BTC","px":"61071.0",
//   "sz":"0.0002","side":"B","time":1795346856581,"startPosition":"0.0","dir":"Open Long",
//   "closedPnl":"0.0","hash":"0x...","oid":613132896783,"crossed":true,"fee":"0.005496",
//   "tid":698930659715340,"cloid":"0x...","feeToken":"USDC","twapId":null}]}}
// "tid" -> Fill::exec_id, "crossed" true = taker (maker = false).
// The first message has "isSnapshot":true (old fills): it is skipped.
std::vector<OrderEvent> parse_user_fills(std::string_view json, const CoinMap& coins);

// Response to a "post" (order / modify / cancel). The post "id" is our RequestId.
// Successes need nothing: "orderUpdates" brings the acks. Only errors matter:
//   order refused:   ..."payload":{"status":"ok","response":{"type":"order","data":
//                         {"statuses":[{"error":"Order must have minimum value of $10."}]}}}
//   cancel refused:  ..."statuses":[{"error":"Order was never placed, already canceled, or filled."}]
//   action refused:  ..."payload":{"status":"err","response":"<text>"}
// Successes look like (real, tests/data/hyperliquid/post_*.json):
//   ..."statuses":[{"resting":{"oid":...}}]    ..."statuses":["success"]    ..."response":{"type":"default"}
struct PostError {
    uint64_t id = 0;   // Our RequestId.
    std::string text;  // Hyperliquid's explanation.
};
// The error, or nullopt if the request succeeded (or this is not a post response).
std::optional<PostError> parse_post_response(std::string_view json);

// The library's reason for one of Hyperliquid's error texts, e.g.
// "Post only order would have immediately matched..." -> PostOnlyWouldCross.
RejectReason reject_reason_from_text(std::string_view text);

// "Order could not immediately match against any resting orders.": an IOC or
// market order found nothing within its price. Not a reject: the order was
// accepted and canceled at once, as on Bybit (seen live 2026-11-22).
bool is_ioc_without_match(std::string_view text);

// POST /info {"type":"orderStatus","user":"0x<account>","oid":"0x<cloid>"}:
//   {"status":"order","order":{"order":{"coin":"BTC",...,"limitPx":"60459.0","sz":"0.0",
//     ...,"origSz":"0.0002",...},"status":"filled","statusTimestamp":1795346857534}}
//   {"status":"unknownOid"}  -> found = false
// (real: tests/data/hyperliquid/order_status_*.json). nullopt if `json` is
// neither (e.g. an HTTP error): the OMS then asks again.
std::optional<OrderStatus> parse_order_status(std::string_view json, const CoinMap& coins, ClientOrderId cid);

// {"type":"orderStatus","user":"0x...","oid":"0x<cloid>"}
std::string make_order_status_request(std::string_view user, std::string_view cloid);

// POST /info {"type":"allMids"}: {"BTC":"61000.5","HYPE":"64.25",...}.
// The mid price of `coin`, or nullopt if it is not there.
std::optional<double> parse_mid(std::string_view json, std::string_view coin);

// POST /info {"type":"meta"}: {"universe":[{"szDecimals":5,"name":"BTC",...}, ...]}
// A coin's asset index (field "a" of orders) is its position in "universe".
struct AssetInfo {
    uint32_t index = 0;
    int sz_decimals = 0;
};
std::optional<std::unordered_map<std::string, AssetInfo>> parse_meta(std::string_view json);

// POST /info {"type":"userRole","user":<api wallet>}:
// {"role":"agent","data":{"user":"0x<main account>"}}. Returns the main
// account if `json` says the address is an agent (API wallet), else nullopt.
std::optional<std::string> parse_agent_owner(std::string_view json);

// --- Formatting ---

// units * step as Hyperliquid wants it: no trailing zeros, no trailing dot.
// format_wire(650003, 0.1) == "65000.3", format_wire(650000, 0.1) == "65000",
// format_wire(100, 0.00001) == "0.001". Same output as the Python SDK's float_to_wire.
std::string format_wire(int64_t units, double step);

// Perp prices: at most 5 significant digits (integers are always fine) and at
// most 6 - szDecimals decimals. Hyperliquid rejects anything else.
bool is_valid_price(std::string_view px, int sz_decimals);

// The tick that keeps prices near `px` valid: the 5th significant digit, but
// no more than 6 - szDecimals decimals. tick_for_price(61000, 5) == 1,
// tick_for_price(3000, 4) == 0.1, tick_for_price(0.1234, 0) == 0.00001.
// Only valid while the price stays in the same power of ten.
double tick_for_price(double px, int sz_decimals);

// Our ClientOrderId as a Hyperliquid cloid: "0x" + 32 hex digits.
std::string make_cloid(uint64_t cid);

// --- From the library to the exchange ---

// {"method":"subscribe","subscription":{"type":"l2Book","coin":"BTC","fast":true}}
// Without "fast":true mainnet sends a book only every ~10 s (seen live on
// 2026-11-14); with it, about every 0.5 s.
std::string make_subscribe_l2book(std::string_view coin);

// The "order" action as JSON (one limit order), e.g.
// {"type":"order","orders":[{"a":0,"b":true,"p":"65000","s":"0.001","r":false,
//   "t":{"limit":{"tif":"Alo"}},"c":"0x00000000000000000000000000000001"}],"grouping":"na"}
// asset: the coin's index in /info "meta". px and sz: already formatted
// ("65000", not "65000.0"). cloid: "0x" + 32 hex digits.
std::string make_order_json(uint32_t asset, bool buy, std::string_view px, std::string_view sz,
                            bool reduce_only, Tif tif, std::string_view cloid);

// The same order as msgpack bytes: this is what gets signed (see hl_signer.hpp).
// Must always describe exactly what make_order_json() sends.
std::string make_order_msgpack(uint32_t asset, bool buy, std::string_view px, std::string_view sz,
                               bool reduce_only, Tif tif, std::string_view cloid);

// An action in both forms: JSON to send, msgpack to sign. Always built together
// so they cannot disagree.
struct Action {
    std::string json;
    std::string msgpack;
};

// The "t" field of an order:
//   limit:   {"limit":{"tif":"Gtc"}}
//   trigger: {"trigger":{"isMarket":true,"triggerPx":"61000","tpsl":"sl"}}
struct OrderKind {
    bool trigger = false;
    Tif tif = Tif::GTC;        // Limit only.
    bool is_market = false;    // Trigger only.
    std::string trigger_px;    // Trigger only, already formatted.
    bool take_profit = false;  // Trigger only: "tp", else "sl".
};

// The library's order type as Hyperliquid's: Market -> limit Ioc (at the
// worst price), Stop* -> trigger "sl", TakeProfit* -> trigger "tp".
// nullopt if Hyperliquid cannot do it (FOK).
std::optional<OrderKind> order_kind(OrderType type, Tif tif, std::string trigger_px);

Action make_order_action(uint32_t asset, bool buy, std::string_view px, std::string_view sz, bool reduce_only,
                         const OrderKind& kind, std::string_view cloid);

// {"type":"modify","oid":"<cloid>","order":{...the whole new order...}}
Action make_modify_action(uint32_t asset, bool buy, std::string_view px, std::string_view sz, bool reduce_only,
                          const OrderKind& kind, std::string_view cloid);

// {"type":"cancelByCloid","cancels":[{"asset":0,"cloid":"0x..."}]}
Action make_cancel_action(uint32_t asset, std::string_view cloid);

// {"method":"subscribe","subscription":{"type":"orderUpdates","user":"0x..."}}
std::string make_subscribe_user(std::string_view type, std::string_view user);

// "post" message containing a signed action (see hl_signer.hpp):
// {"method":"post","id":<n>,"request":{"type":"action","payload":{
//     "action":{...},"nonce":<ms>,"signature":{"r":"0x..","s":"0x..","v":27},"vaultAddress":null}}}
std::string make_post(uint64_t request_id, std::string_view action_json, int64_t nonce, std::string_view r,
                      std::string_view s, int v);

}  // namespace peewee::hyperliquid
