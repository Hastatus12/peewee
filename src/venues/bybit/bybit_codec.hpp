#pragma once
// Translation between Bybit JSON and normalized types.
// Pure functions without networking: test with real messages saved in tests/data/.
// Requires PEEWEE_WITH_NETWORK=ON (simdjson).

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "peewee/core/commands.hpp"
#include "peewee/core/events.hpp"
#include "peewee/core/instrument.hpp"

namespace peewee::bybit {

using SymbolMap = std::unordered_map<std::string, const Instrument*>;

// --- From the exchange to the library ---

// Topic "orderbook.{depth}.{symbol}". Example:
// {"topic":"orderbook.50.BTCUSDT","type":"snapshot","ts":1672304484978,
//  "data":{"s":"BTCUSDT","b":[["16493.50","0.006"]],"a":[["16611.00","0.029"]],
//          "u":18521288,"seq":7961638724},"cts":1672304484976}
// type = "snapshot" | "delta"; quantity "0" means remove the level.
// Treat u == 1 as a new snapshot.
std::optional<BookDelta> parse_orderbook(std::string_view json, const SymbolMap& symbols);

// Topic "order" (private stream): order state changes. One event per order,
// except "New", which yields both OrderAck and AmendAck (Bybit does not say
// which request it answers; the OMS keeps the one matching its pending request).
// Orders with a non-numeric orderLinkId or an unknown symbol are skipped.
std::vector<OrderEvent> parse_order_topic(std::string_view json, const SymbolMap& symbols);

// Topic "execution" (private stream): fills. execId -> Fill::exec_id.
// Only execType "Trade" is a fill of our order (Funding, AdlTrade... are not).
std::vector<OrderEvent> parse_execution_topic(std::string_view json, const SymbolMap& symbols);

// Envelope of any WebSocket message, read without looking at the payload:
// data messages have a topic, control messages (auth, subscribe, ping, trade
// responses) have an op. Bybit reports success either as "success":true
// (public/private streams) or "retCode":0 (trade stream).
struct Envelope {
    std::string topic;
    std::string op;
    bool ok = false;
    std::string text;  // ret_msg / retMsg, for logging.
};
Envelope parse_envelope(std::string_view json);

// REST GET /v5/market/instruments-info?category=...&symbol=... (public).
// nullopt if Bybit does not know the symbol in that category.
struct InstrumentSpec {
    double tick_size = 0.0;  // priceFilter.tickSize
    double lot_size = 0.0;   // lotSizeFilter.qtyStep
    double min_qty = 0.0;    // lotSizeFilter.minOrderQty
};
std::optional<InstrumentSpec> parse_instrument_info(std::string_view json);

// REST GET /v5/order/realtime?...&orderLinkId=<cid>. An empty list means
// Bybit does not know the order (found = false). nullopt on retCode != 0.
std::optional<OrderStatus> parse_order_status(std::string_view json, const SymbolMap& symbols, ClientOrderId cid);

// Response to order.create / order.amend / order.cancel on the trade WS.
// retCode != 0 -> OrderReject with RejectReason mapped from the error code.
// retCode == 0 -> nullopt: Bybit only accepted the request; the "order"
// topic confirms it (OrderAck, AmendAck, OrderClosed).
// reqId is "<cid>-<req>" (see make_create): error responses carry no
// orderLinkId, so the cid has to travel in reqId.
std::optional<OrderEvent> parse_trade_response(std::string_view json);

// --- From the library to the exchange ---

std::string make_subscribe(std::span<const std::string> topics);
std::string make_auth(std::string_view api_key, std::string_view api_secret, int64_t expires_ms);
std::string make_ping();  // {"op":"ping"}, every 20 s on each connection.

// Example trade WS message:
// {"reqId":"42-7","header":{"X-BAPI-TIMESTAMP":"1711001595207","X-BAPI-RECV-WINDOW":"8000"},
//  "op":"order.create","args":[{"symbol":"BTCUSDT","side":"Buy","orderType":"Limit",
//  "qty":"0.001","price":"65000.0","category":"linear","timeInForce":"PostOnly",
//  "orderLinkId":"42"}]}
// orderLinkId is our ClientOrderId; reqId is "<cid>-<req>".
// Market types: "Limit" + "IOC" at the worst price. Trigger types add
// "triggerPrice", "triggerDirection" (1 rise / 2 fall) and "triggerBy":"MarkPrice".
std::string make_create(const NewOrder& o, const Instrument& inst, std::string_view category, int64_t ts_ms);
std::string make_amend(const AmendOrder& o, const Instrument& inst, std::string_view category, int64_t ts_ms);
std::string make_cancel(const CancelOrder& o, const Instrument& inst, std::string_view category, int64_t ts_ms);

}  // namespace peewee::bybit
