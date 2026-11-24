#pragma once
// Tracks all orders, sends them to venues,
// and updates their state when events arrive.
//
// Rules enforced here:
//  - At most ONE request (new, amend, cancel) in progress per order.
//    send_amend / send_cancel return false while another request is pending:
//    the algorithm keeps its desired state and retries after the response.
//  - Responses are matched to the pending request by RequestId; stale
//    responses are dropped and not forwarded to the algorithm.
//  - Duplicate fills (same exec_id) are dropped.
//  - A request with no response within `request_timeout` has an UNKNOWN
//    outcome: the OMS queries the order and trusts the OrderStatus answer.

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "peewee/core/clock.hpp"
#include "peewee/core/events.hpp"
#include "peewee/oms/order.hpp"
#include "peewee/venue/venue.hpp"

namespace peewee {

// Defined outside the class: GCC rejects a nested struct with default member
// initializers as a default argument ("= {}") of the enclosing class.
struct OrderManagerOptions {
    Duration request_timeout = std::chrono::seconds(5);
    // First ClientOrderId. Exchanges remember client IDs (Bybit rejects a
    // reused orderLinkId), so a live process must not restart from 1:
    // the engine seeds it from the wall clock.
    ClientOrderId first_cid = 1;
};

class OrderManager {
public:
    using Options = OrderManagerOptions;

    // Called after an event updates an order: the engine uses it
    // to notify the owning algorithm.
    using Listener = std::function<void(const Order&, const OrderEvent&)>;

    explicit OrderManager(const IClock& clock, Options opt = {})
        : clock_(clock), opt_(opt), next_cid_(opt.first_cid) {}

    void set_listener(Listener l) { listener_ = std::move(l); }

    // Send a new limit order. Return the assigned ClientOrderId.
    ClientOrderId send_new(AlgoId owner, IVenue& venue, InstrumentId inst, Side side,
                           Price px, Qty qty, Tif tif);

    // Send a new order of any type (market, stop, ...). order.cid and
    // order.req are ignored: the OMS assigns them. Return the ClientOrderId.
    ClientOrderId send_new(AlgoId owner, IVenue& venue, NewOrder order);

    // Send an amendment. Return false (and send nothing) if the order does
    // not exist, is closed, or already has a request in progress.
    bool send_amend(ClientOrderId cid, Price px, std::optional<Qty> qty = std::nullopt);

    // Send a cancellation. Same conditions as send_amend.
    bool send_cancel(ClientOrderId cid);

    // Update the order state and notify the listener.
    void on_event(const OrderEvent& e);

    // Detect requests without a response: call regularly (the engine does).
    void check_timeouts();

    // Query every open order of a venue (after a reconnect or lost events).
    void reconcile_venue(VenueId venue);

    // Register a venue to route amend, cancel and query requests.
    void add_venue(IVenue& v) { venues_[v.id()] = &v; }

    const Order* find(ClientOrderId cid) const;

private:
    Order* find_mut(ClientOrderId cid);
    RequestId begin_request(Order& o, RequestKind kind, Price px, std::optional<Qty> qty);
    void clear_pending(Order& o);
    void query(Order& o);
    // Is this response for the request currently pending?
    static bool answers_pending(const Order& o, RequestKind kind, RequestId req);

    const IClock& clock_;
    Options opt_;
    ClientOrderId next_cid_;
    RequestId next_req_ = 1;
    std::unordered_map<ClientOrderId, Order> orders_;
    std::unordered_set<ClientOrderId> pending_cids_;  // Orders with a request in progress.
    // TODO: closed orders and old exec IDs are never removed; prune them
    // periodically in a long-running process.
    std::unordered_set<std::string> seen_execs_;      // "<venue>:<exec_id>"
    std::unordered_map<VenueId, IVenue*> venues_;
    Listener listener_;
};

}  // namespace peewee
