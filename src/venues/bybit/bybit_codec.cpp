#include "bybit_codec.hpp"
#include <simdjson.h>

#include <charconv>

#include "bybit_auth.hpp"

namespace peewee::bybit {

namespace {

// Writes `value` with as many decimals as `step` (the tick or lot size) has.
std::string format_decimal(double value, double step) {
    char buf[512];
    auto res = std::to_chars(buf, buf + sizeof(buf), step, std::chars_format::fixed);
    const std::string_view step_text(buf, static_cast<std::size_t>(res.ptr - buf));
    const auto dot = step_text.find('.');
    const int decimals = dot == std::string_view::npos ? 0 : static_cast<int>(step_text.size() - dot - 1);
    res = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed, decimals);
    return std::string(buf, res.ptr);
}

const char* tif_to_bybit(Tif tif) {
    switch (tif) {
        case Tif::GTC:      return "GTC";
        case Tif::IOC:      return "IOC";
        case Tif::PostOnly: return "PostOnly";
        case Tif::FOK:      return "FOK";
    }
    return "?";
}

// Reads a decimal string sent by Bybit ("16493.50").
bool parse_double(std::string_view text, double& out) {
    const auto res = std::from_chars(text.data(), text.data() + text.size(), out);
    return res.ec == std::errc{} && res.ptr == text.data() + text.size();
}

// Reads levels like [["16493.50","0.006"], ...] and converts them to ticks and lots.
bool parse_levels(simdjson::ondemand::array levels, const Instrument& inst, std::vector<Level>& out) {
    for (auto level_value : levels) {
        simdjson::ondemand::array level;
        if (level_value.get_array().get(level)) {
            return false;
        }

        std::string_view texts[2];
        int count = 0;
        for (auto field : level) {
            if (count == 2 || field.get_string().get(texts[count])) {
                return false;
            }
            ++count;
        }

        double px = 0.0;
        double qty = 0.0;
        if (count != 2 || !parse_double(texts[0], px) || !parse_double(texts[1], qty)) {
            return false;
        }
        out.push_back(Level{inst.price(px), inst.qty(qty)});
    }
    return true;
}

bool parse_uint(std::string_view text, uint64_t& out) {
    const auto res = std::from_chars(text.data(), text.data() + text.size(), out);
    return res.ec == std::errc{} && res.ptr == text.data() + text.size();
}

// Bybit's reqId carries both our ids ("<cid>-<req>"): error responses have
// no orderLinkId, so this is the only way to know which order they refer to.
std::string make_req_id(ClientOrderId cid, RequestId req) {
    return std::to_string(cid) + "-" + std::to_string(req);
}

bool parse_req_id(std::string_view text, ClientOrderId& cid, RequestId& req) {
    const auto dash = text.find('-');
    return dash != std::string_view::npos && parse_uint(text.substr(0, dash), cid) &&
           parse_uint(text.substr(dash + 1), req);
}

bool request_kind_from_op(std::string_view op, RequestKind& kind) {
    if (op == "order.create") {
        kind = RequestKind::New;
    } else if (op == "order.amend") {
        kind = RequestKind::Amend;
    } else if (op == "order.cancel") {
        kind = RequestKind::Cancel;
    } else {
        return false;
    }
    return true;
}

// Codes from https://bybit-exchange.github.io/docs/v5/error
RejectReason reject_reason(int64_t ret_code) {
    switch (ret_code) {
        case 10006: case 10018: case 10429: case 20003:
            return RejectReason::RateLimited;
        case 110001:
            return RejectReason::OrderNotFound;
        case 110004: case 110007: case 110012: case 110044:
            return RejectReason::InsufficientBalance;
        case 110003: case 110120: case 110121:
            return RejectReason::InvalidPrice;
        case 110017:
            return RejectReason::InvalidQty;
        case 10000: case 10016: case 10019:
            return RejectReason::VenueUnavailable;
        default:
            return RejectReason::Unknown;
    }
}

}  // namespace

// TODO: parse with simdjson (on-demand API) and serialize with fmt or
// manual writes into a reused buffer.
// Take care with prices: Bybit sends strings ("16493.50"). Convert them
// to ticks without going through double if exact conversions are required.

std::optional<BookDelta> parse_orderbook(std::string_view json, const SymbolMap& symbols) {
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;

    if (parser.iterate(padded).get(doc)) {
        return std::nullopt;
    }

    // Fields are read in the order Bybit sends them: type, ts, data.
    std::string_view type;
    if (doc["type"].get_string().get(type) || (type != "snapshot" && type != "delta")) {
        return std::nullopt;
    }

    int64_t ts_ms = 0;
    if (doc["ts"].get_int64().get(ts_ms)) {
        return std::nullopt;
    }

    simdjson::ondemand::object data;
    if (doc["data"].get_object().get(data)) {
        return std::nullopt;
    }

    std::string_view symbol;
    if (data["s"].get_string().get(symbol)) {
        return std::nullopt;
    }

    const auto it = symbols.find(std::string(symbol));
    if (it == symbols.end()) {
        return std::nullopt;
    }
    const Instrument& inst = *it->second;

    BookDelta delta;
    delta.inst = inst.id;
    delta.ts = std::chrono::milliseconds(ts_ms);

    simdjson::ondemand::array bids;
    if (data["b"].get_array().get(bids) || !parse_levels(bids, inst, delta.bids)) {
        return std::nullopt;
    }

    simdjson::ondemand::array asks;
    if (data["a"].get_array().get(asks) || !parse_levels(asks, inst, delta.asks)) {
        return std::nullopt;
    }

    uint64_t update_id = 0;
    if (data["u"].get_uint64().get(update_id)) {
        return std::nullopt;
    }
    delta.seq = update_id;
    // u == 1 means Bybit restarted the stream: the message replaces the whole book.
    delta.snapshot = type == "snapshot" || update_id == 1;
    return delta;
}

std::vector<OrderEvent> parse_order_topic(std::string_view json, const SymbolMap& symbols) {
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) {
        return {};
    }

    simdjson::ondemand::array orders;
    if (doc["data"].get_array().get(orders)) {
        return {};
    }

    std::vector<OrderEvent> events;
    for (auto order_value : orders) {
        simdjson::ondemand::object order;
        if (order_value.get_object().get(order)) {
            return {};
        }

        // Fields are read in the order Bybit sends them.
        std::string_view order_id;
        std::string_view link_id;
        std::string_view symbol;
        std::string_view px_text;
        std::string_view qty_text;
        std::string_view status;
        if (order["orderId"].get_string().get(order_id) ||
            order["orderLinkId"].get_string().get(link_id) ||
            order["symbol"].get_string().get(symbol) ||
            order["price"].get_string().get(px_text) ||
            order["qty"].get_string().get(qty_text) ||
            order["orderStatus"].get_string().get(status)) {
            return {};
        }

        // Orders placed outside this library have no numeric orderLinkId: skip them.
        ClientOrderId cid = kNoOrder;
        const auto it = symbols.find(std::string(symbol));
        if (!parse_uint(link_id, cid) || it == symbols.end()) {
            continue;
        }
        const Instrument& inst = *it->second;

        double px = 0.0;
        double qty = 0.0;
        if (!parse_double(px_text, px) || !parse_double(qty_text, qty)) {
            return {};
        }

        // Optional: missing in some messages.
        std::string_view reject_text;
        if (order["rejectReason"].get_string().get(reject_text)) {
            reject_text = {};
        }

        std::string_view updated_text;
        uint64_t updated_ms = 0;
        if (order["updatedTime"].get_string().get(updated_text) || !parse_uint(updated_text, updated_ms)) {
            return {};
        }
        const Timestamp ts = std::chrono::milliseconds(static_cast<int64_t>(updated_ms));

        if (reject_text == "EC_PostOnlyWillTakeLiquidity" || status == "Rejected") {
            // Only the reject, no OrderClosed: the chase retries on PostOnlyWouldCross.
            OrderReject reject;
            reject.cid = cid;
            reject.request = RequestKind::New;
            reject.reason = reject_text == "EC_PostOnlyWillTakeLiquidity" ? RejectReason::PostOnlyWouldCross
                                                                          : RejectReason::Unknown;
            reject.text = std::string(reject_text);
            reject.ts = ts;
            events.push_back(reject);
        } else if (status == "New" || status == "PartiallyFilled" || status == "Untriggered") {
            // (Untriggered: a conditional order, accepted and waiting for its trigger.)
            // The order is in the book. Bybit does not say whether this follows a
            // create or an amend: the OMS keeps the ack matching its pending request
            // and drops the other one.
            OrderAck ack;
            ack.cid = cid;
            ack.venue_oid = std::string(order_id);
            ack.px = inst.price(px);
            ack.qty = inst.qty(qty);
            ack.ts = ts;
            events.push_back(ack);
            // A partial fill can arrive while an amend is pending, carrying the old
            // price: no AmendAck then, the OMS timeout + query resolves it.
            if (status != "PartiallyFilled") {
                events.push_back(AmendAck{.cid = cid, .req = kNoRequest, .px = ack.px, .qty = ack.qty, .ts = ts});
            }
        } else if (status == "Filled") {
            // A cancel that loses the race with a fill produces a second "Filled"
            // message with this reason: the fill was already reported, skip it.
            if (reject_text != "EC_OrigClOrdIDDoesNotExist") {
                events.push_back(OrderClosed{.cid = cid, .req = kNoRequest, .reason = CloseReason::Filled, .ts = ts});
            }
        } else if (status == "Cancelled" || status == "PartiallyFilledCanceled" || status == "Deactivated") {
            events.push_back(OrderClosed{.cid = cid, .req = kNoRequest, .reason = CloseReason::Canceled, .ts = ts});
        }
        // Triggered: a conditional order entering the book; its "New" / "Filled" follows.
    }
    return events;
}

std::vector<OrderEvent> parse_execution_topic(std::string_view json, const SymbolMap& symbols) {
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) {
        return {};
    }

    simdjson::ondemand::array executions;
    if (doc["data"].get_array().get(executions)) {
        return {};
    }

    std::vector<OrderEvent> events;
    for (auto exec_value : executions) {
        simdjson::ondemand::object exec;
        if (exec_value.get_object().get(exec)) {
            return {};
        }

        // Fields are read in the order Bybit sends them.
        std::string_view symbol;
        std::string_view exec_id;
        std::string_view px_text;
        std::string_view qty_text;
        std::string_view exec_type;
        std::string_view link_id;
        std::string_view time_text;
        bool maker = false;
        if (exec["symbol"].get_string().get(symbol) ||
            exec["execId"].get_string().get(exec_id) ||
            exec["execPrice"].get_string().get(px_text) ||
            exec["execQty"].get_string().get(qty_text) ||
            exec["execType"].get_string().get(exec_type) ||
            exec["orderLinkId"].get_string().get(link_id) ||
            exec["execTime"].get_string().get(time_text) ||
            exec["isMaker"].get_bool().get(maker)) {
            return {};
        }

        ClientOrderId cid = kNoOrder;
        const auto it = symbols.find(std::string(symbol));
        if (exec_type != "Trade" || !parse_uint(link_id, cid) || it == symbols.end()) {
            continue;
        }
        const Instrument& inst = *it->second;

        double px = 0.0;
        double qty = 0.0;
        uint64_t time_ms = 0;
        if (!parse_double(px_text, px) || !parse_double(qty_text, qty) || !parse_uint(time_text, time_ms)) {
            return {};
        }

        Fill fill;
        fill.cid = cid;
        fill.exec_id = std::string(exec_id);
        fill.px = inst.price(px);
        fill.qty = inst.qty(qty);
        fill.maker = maker;
        fill.ts = std::chrono::milliseconds(static_cast<int64_t>(time_ms));
        events.push_back(fill);
    }
    return events;
}

Envelope parse_envelope(std::string_view json) {
    Envelope env;
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    simdjson::ondemand::object obj;
    if (parser.iterate(padded).get(doc) || doc.get_object().get(obj)) {
        return env;
    }

    // One pass over the top-level keys: the payload ("data") is skipped, not parsed.
    for (auto field : obj) {
        std::string_view key;
        if (field.unescaped_key().get(key)) {
            break;
        }
        std::string_view text;
        if (key == "topic") {
            if (!field.value().get_string().get(text)) {
                env.topic = std::string(text);
            }
            break;  // Data message: nothing else needed.
        } else if (key == "op") {
            if (!field.value().get_string().get(text)) {
                env.op = std::string(text);
            }
        } else if (key == "success") {
            bool success = false;
            env.ok = !field.value().get_bool().get(success) && success;
        } else if (key == "retCode") {
            int64_t code = -1;
            env.ok = !field.value().get_int64().get(code) && code == 0;
        } else if (key == "ret_msg" || key == "retMsg") {
            if (!field.value().get_string().get(text)) {
                env.text = std::string(text);
            }
        }
    }
    return env;
}

std::optional<InstrumentSpec> parse_instrument_info(std::string_view json) {
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) {
        return std::nullopt;
    }

    int64_t ret_code = -1;
    simdjson::ondemand::array list;
    if (doc["retCode"].get_int64().get(ret_code) || ret_code != 0 ||
        doc["result"]["list"].get_array().get(list)) {
        return std::nullopt;
    }

    for (auto item_value : list) {
        simdjson::ondemand::object item;
        simdjson::ondemand::object price_filter;
        simdjson::ondemand::object lot_filter;
        std::string_view tick_text;
        std::string_view min_text;
        std::string_view step_text;
        if (item_value.get_object().get(item) ||
            item["priceFilter"].get_object().get(price_filter) ||
            price_filter["tickSize"].get_string().get(tick_text) ||
            item["lotSizeFilter"].get_object().get(lot_filter) ||
            lot_filter["minOrderQty"].get_string().get(min_text) ||
            lot_filter["qtyStep"].get_string().get(step_text)) {
            return std::nullopt;
        }

        InstrumentSpec spec;
        if (!parse_double(tick_text, spec.tick_size) || !parse_double(step_text, spec.lot_size) ||
            !parse_double(min_text, spec.min_qty) || spec.tick_size <= 0.0 || spec.lot_size <= 0.0) {
            return std::nullopt;
        }
        return spec;  // Queried by symbol: one item.
    }
    return std::nullopt;  // Empty list: unknown symbol.
}

std::optional<OrderStatus> parse_order_status(std::string_view json, const SymbolMap& symbols, ClientOrderId cid) {
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) {
        return std::nullopt;
    }

    int64_t ret_code = -1;
    simdjson::ondemand::array list;
    if (doc["retCode"].get_int64().get(ret_code) || ret_code != 0 ||
        doc["result"]["list"].get_array().get(list)) {
        return std::nullopt;
    }

    OrderStatus status;
    status.cid = cid;
    for (auto order_value : list) {
        simdjson::ondemand::object order;
        std::string_view link_id;
        std::string_view symbol;
        std::string_view px_text;
        std::string_view qty_text;
        std::string_view state;
        std::string_view filled_text;
        std::string_view updated_text;
        if (order_value.get_object().get(order) ||
            order["orderLinkId"].get_string().get(link_id) ||
            order["symbol"].get_string().get(symbol) ||
            order["price"].get_string().get(px_text) ||
            order["qty"].get_string().get(qty_text) ||
            order["orderStatus"].get_string().get(state) ||
            order["cumExecQty"].get_string().get(filled_text) ||
            order["updatedTime"].get_string().get(updated_text)) {
            return std::nullopt;
        }

        ClientOrderId link_cid = kNoOrder;
        const auto it = symbols.find(std::string(symbol));
        if (!parse_uint(link_id, link_cid) || link_cid != cid || it == symbols.end()) {
            continue;
        }
        const Instrument& inst = *it->second;

        double px = 0.0;
        double qty = 0.0;
        double filled = 0.0;
        uint64_t updated_ms = 0;
        if (!parse_double(px_text, px) || !parse_double(qty_text, qty) || !parse_double(filled_text, filled) ||
            !parse_uint(updated_text, updated_ms)) {
            return std::nullopt;
        }

        status.found = true;
        status.open = state == "New" || state == "PartiallyFilled" || state == "Untriggered";
        status.px = inst.price(px);
        status.qty = inst.qty(qty);
        status.filled = inst.qty(filled);
        status.ts = std::chrono::milliseconds(static_cast<int64_t>(updated_ms));
        break;
    }
    return status;
}

std::optional<OrderEvent> parse_trade_response(std::string_view json) {
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;

    if (parser.iterate(padded).get(doc)) {
        return std::nullopt;
    }

    // Fields are read in the order Bybit sends them: reqId, retCode, retMsg, op, header.
    std::string_view req_id;
    OrderReject reject;
    if (doc["reqId"].get_string().get(req_id) || !parse_req_id(req_id, reject.cid, reject.req)) {
        return std::nullopt;
    }

    int64_t ret_code = 0;
    if (doc["retCode"].get_int64().get(ret_code)) {
        return std::nullopt;
    }
    // Success only means Bybit accepted the request: the order stream confirms it.
    if (ret_code == 0) {
        return std::nullopt;
    }
    reject.reason = reject_reason(ret_code);

    std::string_view ret_msg;
    if (doc["retMsg"].get_string().get(ret_msg)) {
        return std::nullopt;
    }
    reject.text = std::string(ret_msg);

    std::string_view op;
    if (doc["op"].get_string().get(op) || !request_kind_from_op(op, reject.request)) {
        return std::nullopt;
    }

    // Optional: without it the reject still counts, only its timestamp stays 0.
    std::string_view now_text;
    uint64_t now_ms = 0;
    if (!doc["header"]["Timenow"].get_string().get(now_text) && parse_uint(now_text, now_ms)) {
        reject.ts = std::chrono::milliseconds(static_cast<int64_t>(now_ms));
    }
    return reject;
}

std::string make_subscribe(std::span<const std::string> topics) {
    std::size_t capacity = 28;
    for (const auto& topic : topics) {
        capacity += topic.size() + 3;
    }

    std::string result;
    result.reserve(capacity);
    result.append("{\"op\":\"subscribe\",\"args\":[");
    bool first = true;
    constexpr char hex[] = "0123456789abcdef";
    for (const auto& topic : topics) {
        if (!first) {
            result.push_back(',');
        }
        first = false;
        result.push_back('"');
        std::size_t start = 0;
        for (std::size_t i = 0; i < topic.size(); ++i) {
            const auto c = static_cast<unsigned char>(topic[i]);
            if (c == '"' || c == '\\' || c < 0x20) {
                result.append(topic.data() + start, i - start);
                result.push_back('\\');
                if (c == '"' || c == '\\') {
                    result.push_back(static_cast<char>(c));
                } else {
                    result.append("u00");
                    result.push_back(hex[c >> 4]);
                    result.push_back(hex[c & 0x0f]);
                }
                start = i + 1;
            }
        }
        result.append(topic.data() + start, topic.size() - start);
        result.push_back('"');
    }
    result.append("]}");
    return result;
}

std::string make_auth(std::string_view api_key, std::string_view api_secret, int64_t expires_ms) {
    std::string result = "{\"op\":\"auth\",\"args\":[\"";
    result += api_key;
    result += "\",";
    result += std::to_string(expires_ms);
    result += ",\"";
    result += ws_auth_signature(api_secret, expires_ms);
    result += "\"]}";
    return result;
}

std::string make_ping() {
    return "{\"op\":\"ping\"}";
}

std::string make_create(const NewOrder& o, const Instrument& inst, std::string_view category,
                        int64_t ts_ms) {
    std::string result = "{\"reqId\":\"";
    result += make_req_id(o.cid, o.req);
    result += "\",\"header\":{\"X-BAPI-TIMESTAMP\":\"";
    result += std::to_string(ts_ms);
    result += "\",\"X-BAPI-RECV-WINDOW\":\"8000\"},\"op\":\"order.create\",\"args\":[{\"symbol\":\"";
    result += inst.symbol;
    result += "\",\"side\":\"";
    result += to_string(o.side);
    // Always "Limit": the market types are sent as a Limit IOC at their worst
    // price (see OrderType), so they cannot fill at an absurd price.
    result += "\",\"orderType\":\"Limit\",\"qty\":\"";
    result += format_decimal(inst.to_double(o.qty), inst.lot_size);
    result += "\",\"price\":\"";
    result += format_decimal(inst.to_double(o.px), inst.tick_size);
    result += "\",\"category\":\"";
    result += category;
    result += "\",\"timeInForce\":\"";
    result += is_market(o.type) ? "IOC" : tif_to_bybit(o.tif);
    if (is_trigger(o.type)) {
        // Conditional order. triggerDirection 1: when the price rises to
        // triggerPrice, 2: when it falls. Mark price, like Hyperliquid.
        result += "\",\"triggerPrice\":\"";
        result += format_decimal(inst.to_double(o.trigger_px), inst.tick_size);
        result += "\",\"triggerDirection\":";
        result += triggers_on_rise(o.type, o.side == Side::Buy) ? "1" : "2";
        result += ",\"triggerBy\":\"MarkPrice";
    }
    result += "\",\"orderLinkId\":\"";
    result += std::to_string(o.cid);
    result += "\"";
    if (o.reduce_only) {
        result += ",\"reduceOnly\":true";
    }
    result += "}]}";
    return result;
}

std::string make_amend(const AmendOrder& o, const Instrument& inst, std::string_view category,
                       int64_t ts_ms) {
    // Example: 
    // {"reqId":"42-43","header":{"X-BAPI-TIMESTAMP":"1711001595207","X-BAPI-RECV-WINDOW":"8000"},
    // "op":"order.amend","args":[{"symbol":"BTCUSDT","orderLinkId":"42","category":"linear",
    // "price":"65000.5","qty":"0.002"}]}
    std::string result = "{\"reqId\":\"";
    result += make_req_id(o.cid, o.req);
    result += "\",\"header\":{\"X-BAPI-TIMESTAMP\":\"";
    result += std::to_string(ts_ms);
    result += "\",\"X-BAPI-RECV-WINDOW\":\"8000\"},\"op\":\"order.amend\",\"args\":[{\"symbol\":\"";
    result += inst.symbol;
    result += "\",\"orderLinkId\":\"";
    result += std::to_string(o.cid);
    result += "\",\"category\":\"";
    result += category;
    result += "\",\"price\":\"";
    result += format_decimal(inst.to_double(o.px), inst.tick_size);
    result += "\"";
    if (o.qty) {
        result += ",\"qty\":\"";
        result += format_decimal(inst.to_double(*o.qty), inst.lot_size);
        result += "\"";
    }
    result += "}]}";
    return result;
}

std::string make_cancel(const CancelOrder& o, const Instrument& inst, std::string_view category,
                        int64_t ts_ms) {

    std::string result = "{\"reqId\":\"";
    result += make_req_id(o.cid, o.req);
    result += "\",\"header\":{\"X-BAPI-TIMESTAMP\":\"";
    result += std::to_string(ts_ms);
    result += "\",\"X-BAPI-RECV-WINDOW\":\"8000\"},\"op\":\"order.cancel\",\"args\":[{\"symbol\":\"";
    result += inst.symbol;
    result += "\",\"orderLinkId\":\"";
    result += std::to_string(o.cid);
    result += "\",\"category\":\"";
    result += category;
    result += "\"}]}";
    return result;
}

}  // namespace peewee::bybit
