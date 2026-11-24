#pragma once
// State of a single order as tracked by the OMS.

#include <optional>
#include <string>

#include "peewee/core/types.hpp"

namespace peewee {

// Lifecycle of the order itself, i.e. what the exchange holds:
//
//  PendingNew ──► Live ──► Filled
//      │            └────► Canceled
//      └──► Rejected
//
// Requests in progress (amend, cancel) are NOT order states: they are tracked
// in Order::pending. An order stays Live, and can still be filled, while an
// amend or cancel is awaiting its response.
enum class OrderState : uint8_t {
    PendingNew,     // Sent, awaiting confirmation.
    Live,           // In the book (a trigger order: accepted, waiting for its trigger or in the book).
    Filled,         // Fully filled (terminal).
    Canceled,       // Canceled / expired (terminal).
    Rejected,       // Rejected at creation (terminal).
};

constexpr bool is_terminal(OrderState s) {
    return s == OrderState::Filled || s == OrderState::Canceled || s == OrderState::Rejected;
}

constexpr const char* to_string(OrderState s) {
    switch (s) {
        case OrderState::PendingNew: return "PendingNew";
        case OrderState::Live:       return "Live";
        case OrderState::Filled:     return "Filled";
        case OrderState::Canceled:   return "Canceled";
        case OrderState::Rejected:   return "Rejected";
    }
    return "?";
}

// The request currently awaiting a response. At most one per order.
struct PendingRequest {
    RequestKind kind = RequestKind::New;
    RequestId id = kNoRequest;
    Price px;                      // Requested price (New / Amend).
    std::optional<Qty> qty;        // Requested total quantity (Amend), if changed.
    Timestamp sent_at{};           // Monotonic time the request was sent.

    // No response within the timeout: the outcome is UNKNOWN (the exchange
    // may or may not have applied it). The OMS is querying the order state.
    bool unknown = false;
    Timestamp last_query_at{};
};

struct Order {
    ClientOrderId cid = kNoOrder;
    AlgoId owner = 0;              // Algorithm that created this order.
    VenueId venue = 0;
    InstrumentId inst = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Tif tif = Tif::GTC;
    Price trigger_px;              // Trigger orders only.
    bool reduce_only = false;

    Price px;                      // Exchange-confirmed price.
    Qty qty;                       // Total quantity.
    Qty filled;                    // Quantity filled so far.
    OrderState state = OrderState::PendingNew;

    std::optional<PendingRequest> pending;  // Request in progress, if any.
    std::string venue_oid;

    Qty remaining() const { return qty - filled; }
    bool has_pending() const { return pending.has_value(); }
};

}  // namespace peewee
