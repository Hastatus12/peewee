#include "peewee/oms/order_manager.hpp"

#include <algorithm>

#include "peewee/core/log.hpp"

namespace peewee {

ClientOrderId OrderManager::send_new(AlgoId owner, IVenue& venue, InstrumentId inst, Side side,
                                     Price px, Qty qty, Tif tif) {
    NewOrder order;
    order.inst = inst;
    order.side = side;
    order.px = px;
    order.qty = qty;
    order.tif = tif;
    return send_new(owner, venue, order);
}

ClientOrderId OrderManager::send_new(AlgoId owner, IVenue& venue, NewOrder order) {
    venues_[venue.id()] = &venue;

    Order o;
    o.cid = next_cid_++;
    o.owner = owner;
    o.venue = venue.id();
    o.inst = order.inst;
    o.side = order.side;
    o.type = order.type;
    o.tif = order.tif;
    o.trigger_px = order.trigger_px;
    o.reduce_only = order.reduce_only;
    o.px = order.px;
    o.qty = order.qty;
    o.state = OrderState::PendingNew;
    Order& stored = orders_.emplace(o.cid, o).first->second;

    order.cid = stored.cid;
    order.req = begin_request(stored, RequestKind::New, order.px, std::nullopt);
    venue.send_new(order);
    return stored.cid;
}

bool OrderManager::send_amend(ClientOrderId cid, Price px, std::optional<Qty> qty) {
    Order* o = find_mut(cid);
    if (!o || is_terminal(o->state) || o->has_pending()) return false;

    const RequestId req = begin_request(*o, RequestKind::Amend, px, qty);
    venues_.at(o->venue)->send_amend(AmendOrder{.cid = cid, .req = req, .inst = o->inst, .px = px, .qty = qty});
    return true;
}

bool OrderManager::send_cancel(ClientOrderId cid) {
    Order* o = find_mut(cid);
    if (!o || is_terminal(o->state) || o->has_pending()) return false;

    const RequestId req = begin_request(*o, RequestKind::Cancel, o->px, std::nullopt);
    venues_.at(o->venue)->send_cancel(CancelOrder{.cid = cid, .req = req, .inst = o->inst});
    return true;
}

void OrderManager::on_event(const OrderEvent& e) {
    Order* o = find_mut(cid_of(e));
    if (!o) {
        // This can happen for orders created outside this session:
        // in production, reconcile them via REST at startup.
        PEEWEE_LOG("OMS: event for unknown order cid=%llu", static_cast<unsigned long long>(cid_of(e)));
        return;
    }

    bool forward = true;
    std::visit(overloaded{
        [&](const OrderAck& a) {
            if (!answers_pending(*o, RequestKind::New, a.req)) { forward = false; return; }
            clear_pending(*o);
            o->venue_oid = a.venue_oid;
            o->px = a.px;
            if (o->state == OrderState::PendingNew) o->state = OrderState::Live;
        },
        [&](const AmendAck& a) {
            if (!answers_pending(*o, RequestKind::Amend, a.req)) { forward = false; return; }
            // Without a request ID (Bybit, Hyperliquid) an ack only answers OUR
            // amend if it shows the price we asked for. Otherwise it describes
            // an older state of the order, e.g. the "open" of the new order
            // arriving just after the next amend was sent (seen live on
            // Hyperliquid 2026-11-23: two modifies ended up in flight at once).
            if (a.req == kNoRequest && (a.px != o->pending->px ||
                                        (o->pending->qty && a.qty != *o->pending->qty))) {
                forward = false;
                return;
            }
            clear_pending(*o);
            o->px = a.px;
            o->qty = a.qty;
        },
        [&](const OrderReject& r) {
            if (!answers_pending(*o, r.request, r.req)) { forward = false; return; }
            clear_pending(*o);
            if (r.request == RequestKind::New) {
                o->state = OrderState::Rejected;
            } else if (r.reason == RejectReason::OrderNotFound) {
                // The amend/cancel target is gone: probably filled or canceled
                // meanwhile. Ask the exchange instead of guessing.
                query(*o);
            }
            // Otherwise the amend/cancel failed and the order is unchanged.
        },
        [&](const Fill& f) {
            if (!f.exec_id.empty() &&
                !seen_execs_.insert(std::to_string(o->venue) + ":" + f.exec_id).second) {
                forward = false;  // Duplicate execution.
                return;
            }
            o->filled += f.qty;
            if (o->filled >= o->qty) {
                o->state = OrderState::Filled;
                clear_pending(*o);
            }
        },
        [&](const OrderClosed& c) {
            o->state = c.reason == CloseReason::Filled ? OrderState::Filled : OrderState::Canceled;
            clear_pending(*o);
        },
        [&](const OrderStatus& s) {
            clear_pending(*o);
            if (!s.found) {
                // Never reached the exchange, or long gone.
                o->state = o->state == OrderState::PendingNew ? OrderState::Rejected : OrderState::Canceled;
                return;
            }
            o->px = s.px;
            o->qty = s.qty;
            o->filled = std::max(o->filled, s.filled);  // Fills we missed are now counted.
            if (s.open) o->state = OrderState::Live;
            else o->state = o->filled >= o->qty ? OrderState::Filled : OrderState::Canceled;
        },
    }, e);

    if (forward && listener_) listener_(*o, e);
}

void OrderManager::check_timeouts() {
    const Timestamp now = clock_.now();
    for (ClientOrderId cid : pending_cids_) {
        Order& o = orders_.at(cid);
        PendingRequest& p = *o.pending;
        if (!p.unknown && now - p.sent_at >= opt_.request_timeout) {
            PEEWEE_LOG("OMS: %s request for cid=%llu timed out, querying order state",
                       to_string(p.kind), static_cast<unsigned long long>(cid));
            query(o);
        } else if (p.unknown && now - p.last_query_at >= opt_.request_timeout) {
            query(o);  // The query itself got no answer: ask again.
        }
    }
}

void OrderManager::reconcile_venue(VenueId venue) {
    for (auto& [cid, o] : orders_) {
        if (o.venue == venue && !is_terminal(o.state)) query(o);
    }
}

const Order* OrderManager::find(ClientOrderId cid) const {
    auto it = orders_.find(cid);
    return it == orders_.end() ? nullptr : &it->second;
}

Order* OrderManager::find_mut(ClientOrderId cid) {
    auto it = orders_.find(cid);
    return it == orders_.end() ? nullptr : &it->second;
}

RequestId OrderManager::begin_request(Order& o, RequestKind kind, Price px, std::optional<Qty> qty) {
    const RequestId req = next_req_++;
    o.pending = PendingRequest{.kind = kind, .id = req, .px = px, .qty = qty, .sent_at = clock_.now()};
    pending_cids_.insert(o.cid);
    return req;
}

void OrderManager::clear_pending(Order& o) {
    o.pending.reset();
    pending_cids_.erase(o.cid);
}

void OrderManager::query(Order& o) {
    if (o.pending) {
        o.pending->unknown = true;
        o.pending->last_query_at = clock_.now();
    }
    venues_.at(o.venue)->query_order(QueryOrder{.cid = o.cid, .inst = o.inst});
}

bool OrderManager::answers_pending(const Order& o, RequestKind kind, RequestId req) {
    // kNoRequest is accepted for adapters whose exchange cannot echo an ID:
    // with one request per order, the kind is then enough to match.
    return o.pending && o.pending->kind == kind && (req == kNoRequest || req == o.pending->id);
}

}  // namespace peewee
