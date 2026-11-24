#pragma once
// Normalized events: the common format each adapter produces
// by translating messages from its exchange. Above the venues layer,
// no component sees Bybit or Hyperliquid JSON.

#include <string>
#include <variant>
#include <vector>

#include "peewee/core/types.hpp"

namespace peewee {

// ---------------------------------------------------------------------------
// Market data
// ---------------------------------------------------------------------------

struct Level {
    Price px;
    Qty qty;
};

// Order book update.
// - snapshot = true  -> replace the entire book
// - snapshot = false -> delta: each Level sets the quantity at that price;
//                       qty == 0 means remove the level
//
// Performance note: std::vector allocates memory. In production, use a
// fixed-size buffer or a recycled pool. This is sufficient for the skeleton.
struct BookDelta {
    InstrumentId inst = 0;
    uint64_t seq = 0;          // Adapter-normalized sequence number (increasing).
    bool snapshot = false;
    Timestamp ts{};            // Exchange timestamp.
    std::vector<Level> bids;
    std::vector<Level> asks;
};

// ---------------------------------------------------------------------------
// Order events (from the private stream or request responses)
// ---------------------------------------------------------------------------

// Normalized rejection reasons: algorithms react to these rather than
// exchange-specific error codes.
enum class RejectReason : uint8_t {
    Unknown,
    PostOnlyWouldCross,   // The post-only order would have executed immediately.
    RateLimited,          // Too many requests.
    InsufficientBalance,
    InvalidPrice,
    InvalidQty,
    OrderNotFound,        // Order no longer exists (often already filled or canceled).
    VenueUnavailable,     // Disconnected, maintenance, timeout.
};

constexpr const char* to_string(RejectReason r) {
    switch (r) {
        case RejectReason::Unknown:             return "Unknown";
        case RejectReason::PostOnlyWouldCross:  return "PostOnlyWouldCross";
        case RejectReason::RateLimited:         return "RateLimited";
        case RejectReason::InsufficientBalance: return "InsufficientBalance";
        case RejectReason::InvalidPrice:        return "InvalidPrice";
        case RejectReason::InvalidQty:          return "InvalidQty";
        case RejectReason::OrderNotFound:       return "OrderNotFound";
        case RejectReason::VenueUnavailable:    return "VenueUnavailable";
    }
    return "?";
}

enum class CloseReason : uint8_t { Filled, Canceled, Expired };

// The new order was accepted and is in the book.
struct OrderAck {
    ClientOrderId cid = kNoOrder;
    RequestId req = kNoRequest;    // The NewOrder request being answered.
    std::string venue_oid;         // Exchange-assigned ID.
    Price px;
    Qty qty;
    Timestamp ts{};
};

// The amendment was accepted.
struct AmendAck {
    ClientOrderId cid = kNoOrder;
    RequestId req = kNoRequest;    // The AmendOrder request being answered.
    Price px;                      // New price.
    Qty qty;                       // New total order quantity.
    Timestamp ts{};
};

// A request (New / Amend / Cancel) was rejected.
// Warning: rejecting an Amend or Cancel does NOT close the order.
struct OrderReject {
    ClientOrderId cid = kNoOrder;
    RequestId req = kNoRequest;    // The request being answered.
    RequestKind request = RequestKind::New;
    RejectReason reason = RejectReason::Unknown;
    std::string text;              // Original exchange message, for logging.
    Timestamp ts{};
};

// Execution, including partial fills.
// The same execution can be delivered twice (e.g. after a reconnect):
// exec_id lets the OMS discard duplicates.
struct Fill {
    ClientOrderId cid = kNoOrder;
    std::string exec_id;           // Exchange-unique execution ID.
    Price px;
    Qty qty;                       // Quantity executed in THIS fill.
    bool maker = true;
    Timestamp ts{};
};

// The order is no longer in the book (fully filled, canceled, or expired).
struct OrderClosed {
    ClientOrderId cid = kNoOrder;
    RequestId req = kNoRequest;    // The CancelOrder request, if this confirms one.
    CloseReason reason = CloseReason::Canceled;
    Timestamp ts{};
};

// Authoritative state of an order, answering a QueryOrder (reconciliation).
// The OMS sends a query when a request outcome is unknown (timeout,
// OrderNotFound) or after a reconnect, and trusts this answer over anything
// it inferred before.
struct OrderStatus {
    ClientOrderId cid = kNoOrder;
    bool found = false;            // Does the exchange know this order at all?
    bool open = false;             // Still resting in the book?
    Price px;
    Qty qty;                       // Total quantity.
    Qty filled;                    // Cumulative filled quantity.
    Timestamp ts{};
};

using OrderEvent = std::variant<OrderAck, AmendAck, OrderReject, Fill, OrderClosed, OrderStatus>;

// ---------------------------------------------------------------------------
// Connection status
// ---------------------------------------------------------------------------

enum class StreamKind : uint8_t { Public, Private, Trading };

// A connection went down (up = false) or came back (up = true).
// When the Public stream goes down, that venue's books become unreliable.
struct VenueStatus {
    VenueId venue = 0;
    StreamKind stream = StreamKind::Public;
    bool up = false;
    Timestamp ts{};
};

// All events a venue can send to the engine.
using Event = std::variant<BookDelta, OrderEvent, VenueStatus>;

// Helper for std::visit with multiple lambdas.
template <class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

// Every OrderEvent contains a cid.
inline ClientOrderId cid_of(const OrderEvent& e) {
    return std::visit([](const auto& x) { return x.cid; }, e);
}

}  // namespace peewee
