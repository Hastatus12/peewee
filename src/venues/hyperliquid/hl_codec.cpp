#include "hl_codec.hpp"
#include <simdjson.h>
#include "hl_msgpack.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace peewee::hyperliquid {

namespace {

// Reads a decimal string sent by Hyperliquid ("65000.0").
bool parse_double(std::string_view text, double& out) {
    const auto res = std::from_chars(text.data(), text.data() + text.size(), out);
    return res.ec == std::errc{} && res.ptr == text.data() + text.size();
}

// The opposite of make_cloid(): "0x0000000000000000" + 16 hex digits -> cid.
// False for cloids we did not make (e.g. orders placed from the website).
bool parse_cloid(std::string_view cloid, ClientOrderId& cid) {
    if (cloid.size() != 34 || !cloid.starts_with("0x0000000000000000")) {
        return false;
    }
    const std::string_view hex = cloid.substr(18);
    const auto res = std::from_chars(hex.data(), hex.data() + hex.size(), cid, 16);
    return res.ec == std::errc{} && res.ptr == hex.data() + hex.size();
}

const char* tif_to_hyperliquid(Tif tif) {
    switch (tif) {
        case Tif::GTC:      return "Gtc";
        case Tif::IOC:      return "Ioc";
        case Tif::PostOnly: return "Alo";
        case Tif::FOK:      return "?";  // Not on Hyperliquid: order_kind() refuses it.
    }
    return "?";
}

// Reads one side of the book: [{"px":"65000.0","sz":"1.2","n":3}, ...]
// and converts each level to ticks and lots. "n" (order count) is ignored.
bool parse_levels(simdjson::ondemand::array levels, const Instrument& inst, std::vector<Level>& out) {
    for (auto level_value : levels) {
        simdjson::ondemand::object level;
        if (level_value.get_object().get(level)) {
            return false;
        }

        std::string_view px_text;
        std::string_view sz_text;
        if (level["px"].get_string().get(px_text) || level["sz"].get_string().get(sz_text)) {
            return false;
        }

        double px = 0.0;
        double sz = 0.0;
        if (!parse_double(px_text, px) || !parse_double(sz_text, sz)) {
            return false;
        }
        out.push_back(Level{inst.price(px), inst.qty(sz)});
    }
    return true;
}

}  // namespace

std::optional<BookDelta> parse_l2book(std::string_view json, const CoinMap& coins) {
    // {"channel":"l2Book","data":{"coin":"BTC","time":1700000000000,"levels":[[...],[...]]}}
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;

    if (parser.iterate(padded).get(doc)) {
        return std::nullopt;
    }

    std::string_view channel;
    if (doc["channel"].get_string().get(channel) || channel != "l2Book") {
        return std::nullopt;
    }

    simdjson::ondemand::object data;
    if (doc["data"].get_object().get(data)) {
        return std::nullopt;
    }

    std::string_view coin;
    if (data["coin"].get_string().get(coin)) {
        return std::nullopt;
    }

    int64_t ts_ms = 0;
    if (data["time"].get_int64().get(ts_ms)) {
        return std::nullopt;
    }

    const auto it = coins.find(std::string(coin));
    if (it == coins.end()) {
        return std::nullopt;
    }
    const Instrument& inst = *it->second;

    BookDelta delta;
    delta.inst = inst.id;
    delta.ts = std::chrono::milliseconds(ts_ms);
    // Hyperliquid has no sequence number: the time (ms) grows with every message.
    delta.seq = static_cast<uint64_t>(ts_ms);
    // Every message is the full book.
    delta.snapshot = true;

    // "levels" is [bids, asks]: two arrays, in this order.
    simdjson::ondemand::array levels;
    if (data["levels"].get_array().get(levels)) {
        return std::nullopt;
    }

    int side = 0;
    for (auto side_value : levels) {
        simdjson::ondemand::array side_levels;
        if (side == 2 || side_value.get_array().get(side_levels)) {
            return std::nullopt;
        }
        std::vector<Level>& out = side == 0 ? delta.bids : delta.asks;
        if (!parse_levels(side_levels, inst, out)) {
            return std::nullopt;
        }
        ++side;
    }
    if (side != 2) {
        return std::nullopt;
    }

    return delta;
}

std::vector<OrderEvent> parse_order_updates(std::string_view json, const CoinMap& coins, LiveOids& live_oids) {
    // {"channel":"orderUpdates","data":[{"order":{...,"cloid":"0x...28b4"},"status":"open",...}]}
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;

    if (parser.iterate(padded).get(doc)) {
        return {};
    }

    std::string_view channel;
    if (doc["channel"].get_string().get(channel) || channel != "orderUpdates") {
        return {};
    }

    simdjson::ondemand::array updates;
    if (doc["data"].get_array().get(updates)) {
        return {};
    }

    // Steps 1-4 read every update into this list; step 5 turns them into events.
    struct Update {
        ClientOrderId cid;
        uint64_t oid;
        Price px;
        Qty qty;
        std::string status;
        Timestamp ts;
    };
    std::vector<Update> updates_read;
    std::vector<OrderEvent> events;
    for (auto update_value : updates) {
        simdjson::ondemand::object update;
        if (update_value.get_object().get(update)) {
            return {};
        }

        // 1. The "order" object, inside update.
        simdjson::ondemand::object order;
        if (update["order"].get_object().get(order)) {
            return {};
        }

        // 2. Fields inside "order", read in the order Hyperliquid sends them:
        //    coin, side, limitPx, sz, oid, timestamp, origSz, cloid.
        std::string_view coin;
        if (order["coin"].get_string().get(coin)) {
            return {};
        }

        std::string_view px_text;
        if (order["limitPx"].get_string().get(px_text)) {
            return {};
        }

        uint64_t oid = 0;
        if (order["oid"].get_uint64().get(oid)) {
            return {};
        }

        // "sz" is what is still open, "origSz" the whole order: the library
        // wants the whole order.
        std::string_view qty_text;
        if (order["origSz"].get_string().get(qty_text)) {
            return {};
        }

        std::string_view cloid;
        ClientOrderId cid = kNoOrder;
        if (order["cloid"].get_string().get(cloid) || !parse_cloid(cloid, cid)) {
            continue;  // Placed from the website (no cloid, or not ours): skip it.
        }

        // 3. "status" and its time are outside "order": read them from update.
        std::string_view status;
        if (update["status"].get_string().get(status)) {
            return {};
        }

        int64_t status_ms = 0;
        if (update["statusTimestamp"].get_int64().get(status_ms)) {
            return {};
        }
        const Timestamp ts = std::chrono::milliseconds(status_ms);

        // 4. Turn the texts into numbers (ticks and lots).
        const auto it = coins.find(std::string(coin));
        if (it == coins.end()) {
            continue;  // A coin this library does not trade.
        }
        const Instrument& inst = *it->second;

        double px = 0.0;
        double qty = 0.0;
        if (!parse_double(px_text, px) || !parse_double(qty_text, qty)) {
            return {};
        }

        updates_read.push_back(Update{cid, oid, inst.price(px), inst.qty(qty), std::string(status), ts});
    }

    // 5. Create the right events.
    //
    // A modify does NOT move the order: Hyperliquid creates a new oid and
    // cancels the old one, both with our cloid, in the same message (seen
    // live, tests/data/hyperliquid/order_update_modified.json). So the
    // "open" updates go first (they record the current oid), and then a
    // close for an older oid is only the replaced copy: ignore it.
    for (const Update& u : updates_read) {
        if (u.status != "open") continue;
        live_oids[u.cid] = u.oid;
        // Hyperliquid does not say whether this follows a create or a modify:
        // send both, and the OMS keeps the one that matches its pending request.
        OrderAck ack;
        ack.cid = u.cid;
        ack.venue_oid = std::to_string(u.oid);
        ack.px = u.px;
        ack.qty = u.qty;
        ack.ts = u.ts;
        events.push_back(ack);
        events.push_back(AmendAck{.cid = u.cid, .req = kNoRequest, .px = u.px, .qty = u.qty, .ts = u.ts});
    }

    for (const Update& u : updates_read) {
        const auto live = live_oids.find(u.cid);
        // Only an OLDER oid is a replaced copy: oids always grow (see the saved
        // messages). A NEWER unknown oid is still our order, e.g. maybe a
        // trigger order executing under a new oid: its close must count.
        const bool replaced = live != live_oids.end() && u.oid < live->second;

        if (u.status == "filled" || u.status == "canceled" || u.status == "scheduledCancel" ||
            u.status.ends_with("Canceled")) {
            if (replaced) continue;  // The old copy of a modified order.
            // "canceled" is ours; the others are Hyperliquid's (margin, reduce-only, ...).
            const CloseReason reason = u.status == "filled" ? CloseReason::Filled : CloseReason::Canceled;
            events.push_back(OrderClosed{.cid = u.cid, .req = kNoRequest, .reason = reason, .ts = u.ts});
            if (live != live_oids.end()) live_oids.erase(live);
        }
        // "rejected" / "*Rejected" (e.g. "iocCancelRejected", "badAloPxRejected"):
        // skipped. The answer to our post already reported the same error,
        // with its text (seen live 2026-11-22): reporting it here too would
        // give the algorithm two rejects for one request.
        // "triggered": a trigger order entering the book, nothing to report.
    }

    return events;
}

std::vector<OrderEvent> parse_user_fills(std::string_view json, const CoinMap& coins) {
    // {"channel":"userFills","data":{"user":"0x...","fills":[{"coin":"BTC","px":"61071.0",...}]}}
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;

    if (parser.iterate(padded).get(doc)) {
        return {};
    }

    std::string_view channel;
    if (doc["channel"].get_string().get(channel) || channel != "userFills") {
        return {};
    }

    simdjson::ondemand::object data;
    if (doc["data"].get_object().get(data)) {
        return {};
    }

    bool snapshot = false;
    if (!data["isSnapshot"].get_bool().get(snapshot) && snapshot) {
        return {};
    }

    simdjson::ondemand::array fills;
    if (data["fills"].get_array().get(fills)) {
        return {};
    }

    std::vector<OrderEvent> events;
    for (auto fill_value : fills) {
        simdjson::ondemand::object fill;
        if (fill_value.get_object().get(fill)) {
            return {};
        }

        // Fields in the order Hyperliquid sends them:
        // coin, px, sz, side, time, ..., crossed, ..., tid, cloid.
        std::string_view coin;
        if (fill["coin"].get_string().get(coin)) {
            return {};
        }

        std::string_view px_text;
        if (fill["px"].get_string().get(px_text)) {
            return {};
        }

        std::string_view sz_text;
        if (fill["sz"].get_string().get(sz_text)) {
            return {};
        }

        int64_t time_ms = 0;
        if (fill["time"].get_int64().get(time_ms)) {
            return {};
        }

        // true: our order took liquidity (taker).
        bool crossed = false;
        if (fill["crossed"].get_bool().get(crossed)) {
            return {};
        }

        // Unique id of this execution: the OMS uses it to drop duplicates.
        uint64_t tid = 0;
        if (fill["tid"].get_uint64().get(tid)) {
            return {};
        }

        std::string_view cloid;
        ClientOrderId cid = kNoOrder;
        if (fill["cloid"].get_string().get(cloid) || !parse_cloid(cloid, cid)) {
            continue;  // Placed from the website (no cloid, or not ours): skip it.
        }

        const auto it = coins.find(std::string(coin));
        if (it == coins.end()) {
            continue;  // A coin this library does not trade.
        }
        const Instrument& inst = *it->second;

        double px = 0.0;
        double qty = 0.0;
        if (!parse_double(px_text, px) || !parse_double(sz_text, qty)) {
            return {};
        }

        Fill f;
        f.cid = cid;
        f.exec_id = std::to_string(tid);
        f.px = inst.price(px);
        f.qty = inst.qty(qty);
        f.maker = !crossed;
        f.ts = std::chrono::milliseconds(time_ms);
        events.push_back(f);
    }

    return events;
}


std::optional<OrderStatus> parse_order_status(std::string_view json, const CoinMap& coins, ClientOrderId cid) {
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) {
        return std::nullopt;
    }

    std::string_view status;
    if (doc["status"].get_string().get(status)) {
        return std::nullopt;
    }
    if (status == "unknownOid") {
        // Never reached Hyperliquid (or an unknown cloid).
        return OrderStatus{.cid = cid, .found = false, .open = false, .px = {}, .qty = {}, .filled = {}, .ts = {}};
    }
    if (status != "order") {
        return std::nullopt;
    }

    // {"order":{"order":{...},"status":"filled","statusTimestamp":...}}
    simdjson::ondemand::object outer;
    simdjson::ondemand::object order;
    if (doc["order"].get_object().get(outer) || outer["order"].get_object().get(order)) {
        return std::nullopt;
    }

    // Fields in the order Hyperliquid sends them: coin, ..., limitPx, sz, ..., origSz.
    std::string_view coin;
    std::string_view px_text;
    std::string_view left_text;   // "sz": what is still open.
    std::string_view total_text;  // "origSz": the whole order.
    if (order["coin"].get_string().get(coin) || order["limitPx"].get_string().get(px_text) ||
        order["sz"].get_string().get(left_text) || order["origSz"].get_string().get(total_text)) {
        return std::nullopt;
    }

    std::string_view order_status;
    int64_t status_ms = 0;
    if (outer["status"].get_string().get(order_status) || outer["statusTimestamp"].get_int64().get(status_ms)) {
        return std::nullopt;
    }

    const auto it = coins.find(std::string(coin));
    if (it == coins.end()) {
        return std::nullopt;
    }
    const Instrument& inst = *it->second;

    double px = 0.0;
    double left = 0.0;
    double total = 0.0;
    if (!parse_double(px_text, px) || !parse_double(left_text, left) || !parse_double(total_text, total)) {
        return std::nullopt;
    }

    OrderStatus s;
    s.cid = cid;
    s.found = true;
    // "open" (also a trigger order waiting) and "triggered" are still alive;
    // filled / canceled / *Canceled / *Rejected are over.
    s.open = order_status == "open" || order_status == "triggered";
    s.px = inst.price(px);
    s.qty = inst.qty(total);
    s.filled = s.qty - inst.qty(left);
    s.ts = std::chrono::milliseconds(status_ms);
    return s;
}

std::optional<double> parse_mid(std::string_view json, std::string_view coin) {
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    simdjson::ondemand::object mids;
    std::string_view text;
    double mid = 0.0;
    if (parser.iterate(padded).get(doc) || doc.get_object().get(mids) || mids[coin].get_string().get(text) ||
        !parse_double(text, mid) || mid <= 0.0) {
        return std::nullopt;
    }
    return mid;
}

std::string make_order_status_request(std::string_view user, std::string_view cloid) {
    return R"({"type":"orderStatus","user":")" + std::string(user) + R"(","oid":")" + std::string(cloid) + R"("})";
}

std::optional<PostError> parse_post_response(std::string_view json) {
    // {"channel":"post","data":{"id":1,"response":{"type":"action","payload":{"status":"ok", ... "resting" ...}}}}
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;

    if (parser.iterate(padded).get(doc)) {
        return std::nullopt;
    }

    std::string_view channel;
    if (doc["channel"].get_string().get(channel) || channel != "post") {
        return std::nullopt;
    }

    // 2. "data": our request id, then the response.
    simdjson::ondemand::object data;
    uint64_t id = 0;
    if (doc["data"].get_object().get(data) || data["id"].get_uint64().get(id)) {
        return std::nullopt;
    }

    simdjson::ondemand::object response;
    std::string_view response_type;
    if (data["response"].get_object().get(response) || response["type"].get_string().get(response_type)) {
        return std::nullopt;
    }

    // The request itself was refused (e.g. malformed):
    // "response":{"type":"error","payload":"<text>"}
    if (response_type == "error") {
        std::string_view text;
        if (response["payload"].get_string().get(text)) {
            text = "unknown error";
        }
        return PostError{.id = id, .text = std::string(text)};
    }

    // 3. "payload": "status" is "ok" or "err".
    simdjson::ondemand::object payload;
    std::string_view status;
    if (response["payload"].get_object().get(payload) || payload["status"].get_string().get(status)) {
        return std::nullopt;
    }

    // The action was refused: "payload":{"status":"err","response":"<text>"}
    if (status == "err") {
        std::string_view text;
        if (payload["response"].get_string().get(text)) {
            text = "unknown error";
        }
        return PostError{.id = id, .text = std::string(text)};
    }

    // 4. "ok": look for an "error" in response -> data -> statuses.
    // A modify answers just {"type":"default"}, with no "data": nothing to check.
    simdjson::ondemand::object result;
    simdjson::ondemand::object result_data;
    simdjson::ondemand::array statuses;
    if (payload["response"].get_object().get(result) || result["data"].get_object().get(result_data) ||
        result_data["statuses"].get_array().get(statuses)) {
        return std::nullopt;
    }

    for (auto entry : statuses) {
        // An entry is "success" (a text) or an object: {"resting":...}, {"filled":...}, {"error":"..."}.
        simdjson::ondemand::json_type type;
        if (entry.type().get(type) || type != simdjson::ondemand::json_type::object) {
            continue;
        }
        simdjson::ondemand::object object;
        std::string_view text;
        if (!entry.get_object().get(object) && !object["error"].get_string().get(text)) {
            return PostError{.id = id, .text = std::string(text)};
        }
    }

    return std::nullopt;  // Accepted.
}

bool is_ioc_without_match(std::string_view text) {
    return text.find("could not immediately match") != std::string_view::npos;
}

RejectReason reject_reason_from_text(std::string_view text) {
    // Texts as Hyperliquid writes them (see its docs and the Python SDK).
    const auto has = [text](std::string_view part) { return text.find(part) != std::string_view::npos; };
    if (has("Post only order would have immediately matched")) return RejectReason::PostOnlyWouldCross;
    if (has("never placed, already canceled, or filled")) return RejectReason::OrderNotFound;
    if (has("Insufficient margin") || has("Insufficient spot balance")) return RejectReason::InsufficientBalance;
    if (has("minimum value") || has("invalid size")) return RejectReason::InvalidQty;
    if (has("tick size") || has("invalid price") || has("too far from")) return RejectReason::InvalidPrice;
    if (has("Too many") || has("rate limit")) return RejectReason::RateLimited;
    return RejectReason::Unknown;
}

std::optional<std::unordered_map<std::string, AssetInfo>> parse_meta(std::string_view json) {
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    simdjson::ondemand::array universe;
    if (parser.iterate(padded).get(doc) || doc["universe"].get_array().get(universe)) {
        return std::nullopt;
    }

    std::unordered_map<std::string, AssetInfo> assets;
    uint32_t index = 0;
    for (auto entry_value : universe) {
        simdjson::ondemand::object entry;
        int64_t sz_decimals = 0;
        std::string_view name;
        // Fields arrive as szDecimals, then name.
        if (entry_value.get_object().get(entry) || entry["szDecimals"].get_int64().get(sz_decimals) ||
            entry["name"].get_string().get(name)) {
            return std::nullopt;
        }
        assets[std::string(name)] = AssetInfo{index, static_cast<int>(sz_decimals)};
        ++index;
    }
    return assets;
}

std::optional<std::string> parse_agent_owner(std::string_view json) {
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    std::string_view role;
    if (parser.iterate(padded).get(doc) || doc["role"].get_string().get(role) || role != "agent") {
        return std::nullopt;
    }
    std::string_view user;
    if (doc["data"]["user"].get_string().get(user)) {
        return std::nullopt;
    }
    return std::string(user);
}

std::string make_subscribe_l2book(std::string_view coin) {
    std::string result = "{\"method\":\"subscribe\",\"subscription\":{\"type\":\"l2Book\",\"coin\":\"";
    result += coin;
    result += "\",\"fast\":true}}";
    return result;
}

std::string make_subscribe_user(std::string_view type, std::string_view user) {
    std::string result = "{\"method\":\"subscribe\",\"subscription\":{\"type\":\"";
    result += type;
    result += "\",\"user\":\"";
    result += user;
    result += "\"}}";
    return result;
}

std::string make_order_json(uint32_t asset, bool buy, std::string_view px, std::string_view sz,
                            bool reduce_only, Tif tif, std::string_view cloid) {
    // The field order (a, b, p, s, r, t, c) must be the same as in the
    // msgpack that gets signed.
    std::string result = "{\"type\":\"order\",\"orders\":[{";

    result += "\"a\":";
    result += std::to_string(asset);

    result += ",\"b\":";
    result += buy ? "true" : "false";

    result += ",\"p\":\"";
    result += px;
    result += "\"";

    result += ",\"s\":\"";
    result += sz;
    result += "\"";

    result += ",\"r\":";
    result += reduce_only ? "true" : "false";

    result += ",\"t\":{\"limit\":{\"tif\":\"";
    result += tif_to_hyperliquid(tif);
    result += "\"}}";

    result += ",\"c\":\"";
    result += cloid;
    result += "\"";

    result += "}],\"grouping\":\"na\"}";
    return result;
}

std::string make_order_msgpack(uint32_t asset, bool buy, std::string_view px, std::string_view sz,
                               bool reduce_only, Tif tif, std::string_view cloid) {
    // Exactly the same content as make_order_json(), same field order.
    MsgpackWriter w;
    w.map(3);                                   // {type, orders, grouping}
    w.str("type");
    w.str("order");

    w.str("orders");
    w.array(1);                                 // [ one order ]
    w.map(7);                                   // {a, b, p, s, r, t, c}
    w.str("a");
    w.uint(asset);
    w.str("b");
    w.boolean(buy);
    w.str("p");
    w.str(px);
    w.str("s");
    w.str(sz);
    w.str("r");
    w.boolean(reduce_only);
    w.str("t");
    w.map(1);                                   // {"limit": ...}
    w.str("limit");
    w.map(1);                                   // {"tif": ...}
    w.str("tif");
    w.str(tif_to_hyperliquid(tif));
    w.str("c");
    w.str(cloid);

    w.str("grouping");
    w.str("na");
    return w.bytes();
}

namespace {

// One order object {"a":..,"b":..,"p":..,"s":..,"r":..,"t":{..},"c":..},
// as JSON and as msgpack. Field order as in the Python SDK (it is signed).
std::string order_wire_json(uint32_t asset, bool buy, std::string_view px, std::string_view sz, bool reduce_only,
                            const OrderKind& kind, std::string_view cloid) {
    std::string result = "{\"a\":" + std::to_string(asset);
    result += ",\"b\":";
    result += buy ? "true" : "false";
    result += ",\"p\":\"" + std::string(px) + "\"";
    result += ",\"s\":\"" + std::string(sz) + "\"";
    result += ",\"r\":";
    result += reduce_only ? "true" : "false";
    if (kind.trigger) {
        result += ",\"t\":{\"trigger\":{\"isMarket\":";
        result += kind.is_market ? "true" : "false";
        result += ",\"triggerPx\":\"" + kind.trigger_px + "\"";
        result += ",\"tpsl\":\"";
        result += kind.take_profit ? "tp" : "sl";
        result += "\"}}";
    } else {
        result += ",\"t\":{\"limit\":{\"tif\":\"" + std::string(tif_to_hyperliquid(kind.tif)) + "\"}}";
    }
    result += ",\"c\":\"" + std::string(cloid) + "\"}";
    return result;
}

void write_order_wire(MsgpackWriter& w, uint32_t asset, bool buy, std::string_view px, std::string_view sz,
                      bool reduce_only, const OrderKind& kind, std::string_view cloid) {
    w.map(7);
    w.str("a");
    w.uint(asset);
    w.str("b");
    w.boolean(buy);
    w.str("p");
    w.str(px);
    w.str("s");
    w.str(sz);
    w.str("r");
    w.boolean(reduce_only);
    w.str("t");
    w.map(1);
    if (kind.trigger) {
        w.str("trigger");
        w.map(3);
        w.str("isMarket");
        w.boolean(kind.is_market);
        w.str("triggerPx");
        w.str(kind.trigger_px);
        w.str("tpsl");
        w.str(kind.take_profit ? "tp" : "sl");
    } else {
        w.str("limit");
        w.map(1);
        w.str("tif");
        w.str(tif_to_hyperliquid(kind.tif));
    }
    w.str("c");
    w.str(cloid);
}

}  // namespace

std::optional<OrderKind> order_kind(OrderType type, Tif tif, std::string trigger_px) {
    OrderKind kind;
    switch (type) {
        case OrderType::Limit:
            if (tif == Tif::FOK) return std::nullopt;
            kind.tif = tif;
            return kind;
        case OrderType::Market:
            // No market orders on Hyperliquid: an IOC at the worst price is how
            // its own website and SDK do it.
            kind.tif = Tif::IOC;
            return kind;
        case OrderType::StopMarket:
        case OrderType::StopLimit:
        case OrderType::TakeProfitMarket:
        case OrderType::TakeProfitLimit:
            kind.trigger = true;
            kind.is_market = is_market(type);
            kind.trigger_px = std::move(trigger_px);
            kind.take_profit = !is_stop(type);
            return kind;
    }
    return std::nullopt;
}

Action make_order_action(uint32_t asset, bool buy, std::string_view px, std::string_view sz, bool reduce_only,
                         const OrderKind& kind, std::string_view cloid) {
    Action a;
    a.json = "{\"type\":\"order\",\"orders\":[" + order_wire_json(asset, buy, px, sz, reduce_only, kind, cloid) +
             "],\"grouping\":\"na\"}";

    MsgpackWriter w;
    w.map(3);
    w.str("type");
    w.str("order");
    w.str("orders");
    w.array(1);
    write_order_wire(w, asset, buy, px, sz, reduce_only, kind, cloid);
    w.str("grouping");
    w.str("na");
    a.msgpack = w.bytes();
    return a;
}

Action make_modify_action(uint32_t asset, bool buy, std::string_view px, std::string_view sz, bool reduce_only,
                          const OrderKind& kind, std::string_view cloid) {
    Action a;
    a.json = "{\"type\":\"modify\",\"oid\":\"" + std::string(cloid) + "\",\"order\":" +
             order_wire_json(asset, buy, px, sz, reduce_only, kind, cloid) + "}";

    MsgpackWriter w;
    w.map(3);
    w.str("type");
    w.str("modify");
    w.str("oid");
    w.str(cloid);
    w.str("order");
    write_order_wire(w, asset, buy, px, sz, reduce_only, kind, cloid);
    a.msgpack = w.bytes();
    return a;
}

Action make_cancel_action(uint32_t asset, std::string_view cloid) {
    Action a;
    a.json = "{\"type\":\"cancelByCloid\",\"cancels\":[{\"asset\":" + std::to_string(asset) + ",\"cloid\":\"" +
             std::string(cloid) + "\"}]}";

    MsgpackWriter w;
    w.map(2);
    w.str("type");
    w.str("cancelByCloid");
    w.str("cancels");
    w.array(1);
    w.map(2);
    w.str("asset");
    w.uint(asset);
    w.str("cloid");
    w.str(cloid);
    a.msgpack = w.bytes();
    return a;
}

Action make_schedule_cancel_action(std::optional<int64_t> time_ms) {
    // Same fields, in the same order, as the Python SDK's schedule_cancel().
    Action a;
    MsgpackWriter w;
    w.map(time_ms ? 2 : 1);
    w.str("type");
    w.str("scheduleCancel");
    if (time_ms) {
        a.json = "{\"type\":\"scheduleCancel\",\"time\":" + std::to_string(*time_ms) + "}";
        w.str("time");
        w.uint(static_cast<uint64_t>(*time_ms));
    } else {
        a.json = "{\"type\":\"scheduleCancel\"}";
    }
    a.msgpack = w.bytes();
    return a;
}

std::string make_post(uint64_t request_id, std::string_view action_json, int64_t nonce, std::string_view r,
                      std::string_view s, int v) {
    std::string result = "{\"method\":\"post\",\"id\":" + std::to_string(request_id);
    result += ",\"request\":{\"type\":\"action\",\"payload\":{\"action\":";
    result += action_json;
    result += ",\"nonce\":" + std::to_string(nonce);
    result += ",\"signature\":{\"r\":\"" + std::string(r) + "\",\"s\":\"" + std::string(s) +
              "\",\"v\":" + std::to_string(v) + "}";
    result += ",\"vaultAddress\":null}}}";
    return result;
}

// --- Formatting ---

std::string format_wire(int64_t units, double step) {
    char buf[64];
    // Decimals of the step: 0.1 -> 1, 0.00001 -> 5, 1 -> 0.
    auto res = std::to_chars(buf, buf + sizeof(buf), step, std::chars_format::fixed);
    const std::string_view step_text(buf, static_cast<size_t>(res.ptr - buf));
    const auto dot = step_text.find('.');
    const int decimals = dot == std::string_view::npos ? 0 : static_cast<int>(step_text.size() - dot - 1);

    res = std::to_chars(buf, buf + sizeof(buf), static_cast<double>(units) * step, std::chars_format::fixed, decimals);
    std::string text(buf, res.ptr);
    // "65000.30" -> "65000.3", "65000.0" -> "65000".
    if (text.find('.') != std::string::npos) {
        while (text.back() == '0') text.pop_back();
        if (text.back() == '.') text.pop_back();
    }
    return text;
}

bool is_valid_price(std::string_view px, int sz_decimals) {
    const auto dot = px.find('.');
    if (dot == std::string_view::npos) return true;  // Integers are always accepted.
    if (static_cast<int>(px.size() - dot - 1) > 6 - sz_decimals) return false;
    int significant = 0;
    bool leading = true;
    for (char c : px) {
        if (c == '.') continue;
        if (leading && c == '0') continue;
        leading = false;
        ++significant;
    }
    return significant <= 5;
}

std::string make_cloid(uint64_t cid) {
    // 16 bytes = 32 hex digits; our id fills the last 8.
    std::string result = "0x0000000000000000";
    static constexpr char digits[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) result += digits[(cid >> (4 * i)) & 0x0f];
    return result;
}

}  // namespace peewee::hyperliquid
