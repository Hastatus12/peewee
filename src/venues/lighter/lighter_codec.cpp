#include "lighter_codec.hpp"

#include <simdjson.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace peewee::lighter {

namespace {

namespace od = simdjson::ondemand;

// Keeps the parser state alive with the views it hands out. One parser per
// thread, reused: creating one allocates its buffers. Only one Doc may be
// alive per thread at a time (iterate() invalidates the previous document).
struct Doc {
    explicit Doc(std::string_view json) : padded(json) {}
    simdjson::padded_string padded;
    od::document doc;
    bool ok() {
        thread_local od::parser parser;
        return !parser.iterate(padded).get(doc);
    }
};

bool get_scaled(simdjson::simdjson_result<od::value> v, int decimals, int64_t& out) {
    std::string_view text;
    if (v.get_string().get(text)) return false;
    const auto n = parse_scaled(text, decimals);
    if (!n) return false;
    out = *n;
    return true;
}

// [{"price":"60593.9","size":"0.42497"}, ...] -> ticks and lots.
bool parse_levels(od::array levels, int px_decimals, int qty_decimals, std::vector<Level>& out) {
    for (auto item : levels) {
        od::object level;
        int64_t px = 0, qty = 0;
        if (item.get_object().get(level) || !get_scaled(level["price"], px_decimals, px) ||
            !get_scaled(level["size"], qty_decimals, qty)) {
            return false;
        }
        out.push_back(Level{Price{px}, Qty{qty}});
    }
    return true;
}

// The fields of one Order JSON that the library needs.
struct RawOrder {
    int64_t coi = 0;
    int64_t market = 0;
    std::string order_index;
    std::string_view status;
    int64_t px = 0;
    int64_t total = 0;   // initial_base_amount
    int64_t filled = 0;  // filled_base_amount
    Timestamp ts{};
};

bool is_open_status(std::string_view status) {
    return status == "open" || status == "pending" || status == "in-progress";
}

bool read_order(od::object order, const MarketMap& markets, RawOrder& out) {
    int64_t order_index = 0;
    if (order["order_index"].get_int64().get(order_index) ||
        order["client_order_index"].get_int64().get(out.coi) ||
        order["market_index"].get_int64().get(out.market)) {
        return false;
    }
    const auto it = markets.find(out.market);
    if (it == markets.end()) return false;
    const int px_dec = it->second.px_dec;
    const int qty_dec = it->second.qty_dec;
    int64_t remaining = 0;
    if (!get_scaled(order["initial_base_amount"], qty_dec, out.total) ||
        !get_scaled(order["price"], px_dec, out.px) ||
        !get_scaled(order["remaining_base_amount"], qty_dec, remaining) ||
        !get_scaled(order["filled_base_amount"], qty_dec, out.filled) ||
        order["status"].get_string().get(out.status)) {
        return false;
    }
    // An open order's size is filled + remaining: that follows a modify, which
    // changes the order's amount (its total, filled part included). A closed
    // order has nothing remaining: keep initial_base_amount.
    if (is_open_status(out.status)) out.total = out.filled + remaining;
    // transaction_time (us) when present, else timestamp (s).
    int64_t us = 0;
    int64_t s = 0;
    if (!order["transaction_time"].get_int64().get(us) && us > 0) {
        out.ts = std::chrono::microseconds(us);
    } else if (!order["timestamp"].get_int64().get(s)) {
        out.ts = std::chrono::seconds(s);
    }
    out.order_index = std::to_string(order_index);
    return true;
}

void order_events(const RawOrder& o, std::vector<OrderEvent>& events) {
    const ClientOrderId cid = static_cast<ClientOrderId>(o.coi);
    const std::string_view st = o.status;
    if (st == "open" || st == "pending") {
        events.push_back(OrderAck{.cid = cid, .req = kNoRequest, .venue_oid = o.order_index, .px = Price{o.px},
                                  .qty = Qty{o.total}, .ts = o.ts});
        events.push_back(AmendAck{.cid = cid, .req = kNoRequest, .px = Price{o.px}, .qty = Qty{o.total}, .ts = o.ts});
    } else if (st == "filled") {
        events.push_back(OrderClosed{.cid = cid, .req = kNoRequest, .reason = CloseReason::Filled, .ts = o.ts});
    } else if (st == "canceled-post-only") {
        // Only the reject, no OrderClosed: the chase retries on PostOnlyWouldCross (as on Bybit).
        events.push_back(OrderReject{.cid = cid, .req = kNoRequest, .request = RequestKind::New,
                                     .reason = RejectReason::PostOnlyWouldCross, .text = std::string(st), .ts = o.ts});
    } else if (st == "canceled-expired") {
        events.push_back(OrderClosed{.cid = cid, .req = kNoRequest, .reason = CloseReason::Expired, .ts = o.ts});
    } else if (st.starts_with("canceled")) {
        events.push_back(OrderClosed{.cid = cid, .req = kNoRequest, .reason = CloseReason::Canceled, .ts = o.ts});
    }
    // in-progress: being registered, an "open" (or a cancel) follows.
}

}  // namespace

std::optional<int64_t> parse_scaled(std::string_view text, int decimals) {
    if (text.empty() || decimals < 0 || decimals > 18) return std::nullopt;
    bool negative = false;
    if (text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    int64_t value = 0;
    int frac_digits = -1;  // -1: no '.' seen yet.
    bool any_digit = false;
    for (char c : text) {
        if (c == '.') {
            if (frac_digits >= 0) return std::nullopt;
            frac_digits = 0;
            continue;
        }
        if (c < '0' || c > '9') return std::nullopt;
        any_digit = true;
        if (frac_digits >= decimals) {
            if (c != '0') return std::nullopt;  // Finer than one step.
            continue;
        }
        if (value > (INT64_MAX - 9) / 10) return std::nullopt;
        value = value * 10 + (c - '0');
        if (frac_digits >= 0) ++frac_digits;
    }
    if (!any_digit) return std::nullopt;
    for (int i = std::max(frac_digits, 0); i < decimals; ++i) {
        if (value > INT64_MAX / 10) return std::nullopt;
        value *= 10;
    }
    return negative ? -value : value;
}

int decimals_of(double step) {
    int d = 0;
    while (d < 18 && std::abs(step * std::pow(10.0, d) - std::round(step * std::pow(10.0, d))) > 1e-9) ++d;
    return d;
}

std::optional<BookUpdate> parse_order_book(std::string_view json, const MarketMap& markets) {
    Doc d(json);
    if (!d.ok()) return std::nullopt;

    std::string_view channel;
    if (d.doc["channel"].get_string().get(channel) || !channel.starts_with("order_book:")) return std::nullopt;
    const auto market = parse_scaled(channel.substr(11), 0);
    if (!market) return std::nullopt;
    const auto it = markets.find(*market);
    if (it == markets.end()) return std::nullopt;
    const Instrument& inst = *it->second.inst;
    const int px_dec = it->second.px_dec;
    const int qty_dec = it->second.qty_dec;

    BookUpdate u;
    u.delta.inst = inst.id;
    od::object book;
    od::array asks, bids;
    int64_t nonce = 0;
    if (d.doc["order_book"].get_object().get(book) || book["asks"].get_array().get(asks) ||
        !parse_levels(asks, px_dec, qty_dec, u.delta.asks) || book["bids"].get_array().get(bids) ||
        !parse_levels(bids, px_dec, qty_dec, u.delta.bids) || book["nonce"].get_int64().get(nonce)) {
        return std::nullopt;
    }
    int64_t begin = 0;
    if (book["begin_nonce"].get_int64().get(begin)) begin = 0;

    int64_t ts_ms = 0;
    std::string_view type;
    if (d.doc["timestamp"].get_int64().get(ts_ms) || d.doc["type"].get_string().get(type)) return std::nullopt;

    u.delta.seq = static_cast<uint64_t>(nonce);
    u.delta.ts = std::chrono::milliseconds(ts_ms);
    u.delta.snapshot = type == "subscribed/order_book";
    u.begin_nonce = u.delta.snapshot ? 0 : static_cast<uint64_t>(begin);
    return u;
}

std::vector<OrderEvent> parse_account_orders(std::string_view json, const MarketMap& markets) {
    std::vector<OrderEvent> events;
    Doc d(json);
    if (!d.ok()) return events;

    std::string_view channel;
    od::object by_market;
    if (d.doc["channel"].get_string().get(channel) || !channel.starts_with("account_all_orders:") ||
        d.doc["orders"].get_object().get(by_market)) {
        return events;
    }
    for (auto field : by_market) {
        od::field f;
        od::array orders;
        if (std::move(field).get(f) || f.value().get_array().get(orders)) continue;
        for (auto item : orders) {
            od::object order;
            RawOrder o;
            if (item.get_object().get(order) || !read_order(order, markets, o)) continue;
            order_events(o, events);
        }
    }
    return events;
}

std::vector<OrderEvent> parse_account_trades(std::string_view json, const MarketMap& markets, int64_t account) {
    std::vector<OrderEvent> events;
    Doc d(json);
    if (!d.ok()) return events;

    std::string_view channel;
    if (d.doc["channel"].get_string().get(channel) || !channel.starts_with("account_all_trades:")) return events;
    // "type" is the last field: look for it in the text rather than parse twice.
    if (json.find(R"("type":"update/account_all_trades")") == std::string_view::npos) return events;

    od::object by_market;
    if (d.doc["trades"].get_object().get(by_market)) return events;
    for (auto field : by_market) {
        od::field f;
        od::array trades;
        if (std::move(field).get(f) || f.value().get_array().get(trades)) continue;
        for (auto item : trades) {
            od::object t;
            int64_t trade_id = 0, market = 0, ask_client = 0, bid_client = 0, ask_account = 0, bid_account = 0;
            int64_t ts_ms = 0;
            bool maker_is_ask = false;
            if (item.get_object().get(t) || t["trade_id"].get_int64().get(trade_id) ||
                t["market_id"].get_int64().get(market)) {
                continue;
            }
            const auto it = markets.find(market);
            if (it == markets.end()) continue;
            int64_t qty = 0, px = 0;
            if (!get_scaled(t["size"], it->second.qty_dec, qty) || !get_scaled(t["price"], it->second.px_dec, px) ||
                t["ask_client_id"].get_int64().get(ask_client) || t["bid_client_id"].get_int64().get(bid_client) ||
                t["ask_account_id"].get_int64().get(ask_account) ||
                t["bid_account_id"].get_int64().get(bid_account) || t["is_maker_ask"].get_bool().get(maker_is_ask) ||
                t["timestamp"].get_int64().get(ts_ms)) {
                continue;
            }
            const std::string id = std::to_string(trade_id);
            const Timestamp ts = std::chrono::milliseconds(ts_ms);
            if (ask_account == account) {
                events.push_back(Fill{.cid = static_cast<ClientOrderId>(ask_client), .exec_id = id + "a",
                                      .px = Price{px}, .qty = Qty{qty}, .maker = maker_is_ask, .ts = ts});
            }
            if (bid_account == account) {
                events.push_back(Fill{.cid = static_cast<ClientOrderId>(bid_client), .exec_id = id + "b",
                                      .px = Price{px}, .qty = Qty{qty}, .maker = !maker_is_ask, .ts = ts});
            }
        }
    }
    return events;
}

std::optional<ErrorMessage> parse_error(std::string_view json) {
    Doc d(json);
    if (!d.ok()) return std::nullopt;
    od::object error;
    if (d.doc["error"].get_object().get(error)) return std::nullopt;

    ErrorMessage e;
    int64_t code = 0;
    std::string_view text;
    if (!error["code"].get_int64().get(code)) e.code = static_cast<int>(code);
    if (!error["message"].get_string().get(text)) e.text = std::string(text);

    // The id we sent is a string; accept a number too.
    od::value id;
    if (!d.doc["id"].get(id)) {
        std::string_view id_text;
        int64_t id_num = 0;
        if (!id.get_string().get(id_text)) e.id = std::string(id_text);
        else if (!id.get_int64().get(id_num)) e.id = std::to_string(id_num);
    }
    return e;
}

RejectReason reject_reason_from_code(int code) {
    switch (code) {
        case 21506:  // too many pending txs
        case 23000:  // Too Many Requests
        case 30009:  // Too Many Websocket Messages
        case 30010:  // Too Many Inflight Messages
            return RejectReason::RateLimited;
        case 21304:  // not enough asset balance
        case 21305:
        case 21507:  // below maintenance margin
        case 21508:  // below initial margin
        case 21739:  // not enough margin to create the order
            return RejectReason::InsufficientBalance;
        case 21702:  // invalid price
        case 21729:  // invalid order trigger price
        case 21733:  // fat finger price
        case 21734:  // too far from the mark price
        case 21735:  // SL/TP price too far from the trigger
        case 21753:  // too far from the index price
            return RejectReason::InvalidPrice;
        case 21701:  // invalid base amount
        case 21706:  // invalid order base or quote amount
            return RejectReason::InvalidQty;
        case 21700:  // invalid order index
        case 21709:  // order is inactive
        case 21715:  // given order is not an active order
        case 21737:  // invalid order status
            return RejectReason::OrderNotFound;
        default:
            return RejectReason::Unknown;
    }
}

bool is_nonce_error(int code) { return code == 21104 || code == 21101; }

std::optional<OrderStatus> parse_order_query(std::string_view json, const MarketMap& markets, int64_t coi) {
    Doc d(json);
    if (!d.ok()) return std::nullopt;
    od::array orders;
    if (d.doc["orders"].get_array().get(orders)) return std::nullopt;

    OrderStatus status{.cid = static_cast<ClientOrderId>(coi), .found = false, .open = false,
                       .px = {}, .qty = {}, .filled = {}, .ts = {}};
    for (auto item : orders) {
        od::object order;
        RawOrder o;
        if (item.get_object().get(order) || !read_order(order, markets, o) || o.coi != coi) continue;
        status.found = true;
        status.open = is_open_status(o.status);
        status.px = Price{o.px};
        status.qty = Qty{o.total};
        status.filled = Qty{o.filled};
        status.ts = o.ts;
        break;
    }
    return status;
}

std::optional<std::unordered_map<std::string, MarketInfo>> parse_market_details(std::string_view json) {
    Doc d(json);
    if (!d.ok()) return std::nullopt;
    od::array list;
    if (d.doc["order_book_details"].get_array().get(list)) return std::nullopt;

    std::unordered_map<std::string, MarketInfo> out;
    for (auto item : list) {
        od::object m;
        std::string_view symbol, type, min_text;
        int64_t id = 0, size_dec = 0, price_dec = 0;
        if (item.get_object().get(m) || m["symbol"].get_string().get(symbol) || m["market_id"].get_int64().get(id) ||
            m["market_type"].get_string().get(type) || m["min_base_amount"].get_string().get(min_text) ||
            m["size_decimals"].get_int64().get(size_dec) || m["price_decimals"].get_int64().get(price_dec)) {
            return std::nullopt;
        }
        if (type != "perp") continue;
        const auto min_base = parse_scaled(min_text, static_cast<int>(size_dec));
        if (!min_base) return std::nullopt;
        out[std::string(symbol)] = MarketInfo{
            .market_id = id,
            .size_decimals = static_cast<int>(size_dec),
            .price_decimals = static_cast<int>(price_dec),
            .min_base_amount = static_cast<double>(*min_base) * std::pow(10.0, -static_cast<double>(size_dec)),
        };
    }
    return out;
}

std::optional<std::string> parse_api_key(std::string_view json) {
    Doc d(json);
    if (!d.ok()) return std::nullopt;
    od::array keys;
    if (d.doc["api_keys"].get_array().get(keys)) return std::nullopt;
    for (auto item : keys) {
        std::string_view pk;
        if (!item["public_key"].get_string().get(pk) && !pk.empty()) return std::string(pk);
    }
    return std::nullopt;
}

std::optional<int64_t> parse_next_nonce(std::string_view json) {
    Doc d(json);
    int64_t code = 0, nonce = 0;
    if (!d.ok() || d.doc["code"].get_int64().get(code) || code != 200 || d.doc["nonce"].get_int64().get(nonce)) {
        return std::nullopt;
    }
    return nonce;
}

std::optional<OrderSpec> order_spec(OrderType type, Tif tif) {
    constexpr uint8_t kIoc = 0, kGtt = 1, kPostOnly = 2;
    switch (type) {
        case OrderType::Limit:
            switch (tif) {
                case Tif::GTC: return OrderSpec{0, kGtt, true};
                case Tif::PostOnly: return OrderSpec{0, kPostOnly, true};
                case Tif::IOC: return OrderSpec{0, kIoc, false};
                case Tif::FOK: return std::nullopt;
            }
            return std::nullopt;
        case OrderType::Market: return OrderSpec{1, kIoc, false};
        case OrderType::StopMarket: return OrderSpec{2, kIoc, true};
        case OrderType::StopLimit: return OrderSpec{3, kGtt, true};
        case OrderType::TakeProfitMarket: return OrderSpec{4, kIoc, true};
        case OrderType::TakeProfitLimit: return OrderSpec{5, kGtt, true};
    }
    return std::nullopt;
}

std::string make_subscribe(std::string_view channel) {
    return R"({"type":"subscribe","channel":")" + std::string(channel) + R"("})";
}

std::string make_subscribe(std::string_view channel, std::string_view auth) {
    return R"({"type":"subscribe","channel":")" + std::string(channel) + R"(","auth":")" + std::string(auth) + R"("})";
}

std::string make_unsubscribe(std::string_view channel) {
    return R"({"type":"unsubscribe","channel":")" + std::string(channel) + R"("})";
}

std::string make_send_tx(uint64_t id, uint8_t tx_type, std::string_view tx_info) {
    return R"({"type":"jsonapi/sendtx","data":{"id":")" + std::to_string(id) + R"(","tx_type":)" +
           std::to_string(unsigned{tx_type}) + R"(,"tx_info":)" + std::string(tx_info) + "}}";
}

}  // namespace peewee::lighter
