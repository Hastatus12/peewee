#pragma once
// Base class for all execution algorithms.
//
// Rules for algorithm implementations:
//  - NEVER perform I/O: no sockets, files, or sleeps;
//  - react only to events (on_book, on_order, on_timer, ...);
//  - send orders ONLY through ctx_.oms;
//  - obtain time ONLY from ctx_.clock.
// This lets the same algorithm run identically on real exchanges and in simulation.

#include "peewee/book/order_book.hpp"
#include "peewee/core/clock.hpp"
#include "peewee/core/events.hpp"
#include "peewee/core/instrument.hpp"
#include "peewee/oms/order_manager.hpp"
#include "peewee/venue/venue.hpp"

namespace peewee {

class Engine;

// Everything an algorithm can access.
struct AlgoContext {
    OrderManager& oms;
    IVenue& venue;
    const IClock& clock;
    const Instrument& instrument;
    const OrderBook& book;
};

class Algo {
public:
    explicit Algo(AlgoContext ctx) : ctx_(ctx) {}
    virtual ~Algo() = default;

    Algo(const Algo&) = delete;
    Algo& operator=(const Algo&) = delete;

    AlgoId id() const { return id_; }

    // Called once, immediately after creation.
    virtual void on_start() {}

    // The instrument's order book changed.
    // WARNING: may also be called with an invalid book (book.valid() == false).
    virtual void on_book(const OrderBook& /*book*/) {}

    // An owned order received an event. `order` has already been updated by the OMS.
    virtual void on_order(const Order& /*order*/, const OrderEvent& /*event*/) {}

    // Called periodically by the engine (see Engine::set_timer_interval).
    virtual void on_timer() {}

    // A connection to the algorithm's venue went down or came back.
    virtual void on_venue_status(const VenueStatus& /*status*/) {}

    // External stop request (e.g. the user wants to stop the algorithm).
    virtual void stop() {}

    // True when the algorithm has finished its work.
    virtual bool done() const = 0;

protected:
    Timestamp now() const { return ctx_.clock.now(); }

    AlgoContext ctx_;

private:
    friend class Engine;
    AlgoId id_ = 0;
};

}  // namespace peewee
