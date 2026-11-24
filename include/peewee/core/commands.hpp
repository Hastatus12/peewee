#pragma once
// Normalized commands sent by the engine/OMS to adapters.
// Each adapter translates them into its exchange's format.
//
// `req` identifies the request: the adapter must copy it into the matching
// response event (OrderAck, AmendAck, OrderReject, or the OrderClosed that
// confirms a cancel), so the OMS can tell a current response from a stale one.

#include <optional>

#include "peewee/core/types.hpp"

namespace peewee {

struct NewOrder {
    ClientOrderId cid = kNoOrder;
    RequestId req = kNoRequest;
    InstrumentId inst = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price px{};                     // Limit price, or worst price for market types (see OrderType).
    Qty qty{};
    Tif tif = Tif::GTC;             // Limit orders only: market types are always IOC,
                                    // trigger limit orders GTC once triggered.
    Price trigger_px{};             // Trigger orders only.
    bool reduce_only = false;
};

struct AmendOrder {
    ClientOrderId cid = kNoOrder;
    RequestId req = kNoRequest;
    InstrumentId inst = 0;
    Price px;                       // New price.
    std::optional<Qty> qty;         // New total quantity; nullopt = unchanged.
};

struct CancelOrder {
    ClientOrderId cid = kNoOrder;
    RequestId req = kNoRequest;
    InstrumentId inst = 0;
};

// Ask the exchange for the authoritative state of one order.
// The answer arrives as an OrderStatus event.
struct QueryOrder {
    ClientOrderId cid = kNoOrder;
    InstrumentId inst = 0;
};

}  // namespace peewee
