#pragma once
// Interface that EVERY exchange adapter must implement.
// Higher layers (OMS, algorithms, engine) depend only on IVenue
// and do not know which exchange is behind it.

#include <string_view>

#include "peewee/core/commands.hpp"
#include "peewee/core/events.hpp"
#include "peewee/core/instrument.hpp"
#include "peewee/venue/capabilities.hpp"

namespace peewee {

// Recipient of venue events (implemented by the engine).
//
// THREADING CONTRACT: for a given venue, on_event must always be called from
// ONE single thread (the engine uses a single-producer queue per venue).
// An adapter with several connections (Bybit has three WebSockets) must run
// all of them on the same network thread, e.g. one shared asio::io_context.
class IEventSink {
public:
    virtual ~IEventSink() = default;
    virtual void on_event(Event&& e) = 0;
};

class IVenue {
public:
    virtual ~IVenue() = default;

    virtual VenueId id() const = 0;
    virtual std::string_view name() const = 0;
    virtual const VenueCapabilities& caps() const = 0;

    // Event destination. Called by the engine before connect().
    virtual void set_sink(IEventSink* sink) = 0;

    // Register an instrument: the adapter stores the mapping
    // InstrumentId <-> exchange symbol.
    virtual void add_instrument(const Instrument& inst) = 0;

    virtual void connect() = 0;
    // Must stop and JOIN the network thread before returning:
    // after disconnect() returns, on_event is never called again.
    virtual void disconnect() = 0;

    // Start the order book stream. The first event must be a snapshot.
    virtual void subscribe_book(InstrumentId inst, int depth) = 0;
    // Start the private stream (orders, fills). Requires authentication.
    virtual void subscribe_private() = 0;

    // Send requests. They are ASYNCHRONOUS: responses arrive later as
    // events (OrderAck / AmendAck / OrderReject / OrderClosed) carrying
    // the same `req`. Called from the engine thread; the adapter forwards
    // them to its network thread if necessary.
    virtual void send_new(const NewOrder& o) = 0;
    virtual void send_amend(const AmendOrder& o) = 0;
    virtual void send_cancel(const CancelOrder& o) = 0;

    // Ask for the authoritative state of an order (REST or WS query).
    // The answer arrives as an OrderStatus event.
    virtual void query_order(const QueryOrder& q) = 0;

    // The engine lost events from this venue (queue overflow): rebuild
    // everything from scratch, i.e. send a fresh book snapshot for every
    // subscribed instrument, then VenueStatus{up = true} for each stream.
    virtual void resync() = 0;

    // Does the rate limiter have budget for this request type?
    virtual bool can_send(RequestKind kind) const = 0;

    // Called by the engine on each loop iteration. Useful for adapters
    // running on the engine thread (or for the simulator).
    virtual void poll() {}
};

}  // namespace peewee
