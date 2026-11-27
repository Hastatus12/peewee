#pragma once
// Receives events from all venues, updates books and the OMS,
// and notifies interested algorithms. Runs on ONE thread,
// one event at a time: no locks and deterministic behavior.

#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "peewee/algo/algo.hpp"
#include "peewee/book/order_book.hpp"
#include "peewee/core/clock.hpp"
#include "peewee/core/spsc_queue.hpp"
#include "peewee/oms/order_manager.hpp"
#include "peewee/venue/venue.hpp"

namespace peewee {

// Defined outside the class for the same GCC reason as OrderManagerOptions.
struct EngineOptions {
    size_t queue_capacity = 1 << 16;                    // Events buffered per venue.
    size_t max_events_per_venue = 256;                  // Per poll(): a busy venue cannot starve the others.
    Duration timer_interval = std::chrono::milliseconds(10);  // Algorithm on_timer() period (0 = never).
    Duration request_timeout = std::chrono::seconds(5); // After this, a request outcome is unknown.
};

class Engine {
public:
    using Options = EngineOptions;

    explicit Engine(Options opt = {});                          // Use the real clock.
    explicit Engine(const IClock& clock, Options opt = {});     // Use an external clock (tests, simulation).
    ~Engine();  // Calls shutdown().

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Create a venue. V's constructor receives (VenueId, args...).
    template <class V, class... Args>
    V& add_venue(Args&&... args) {
        const auto id = static_cast<VenueId>(venues_.size());
        auto v = std::make_unique<V>(id, std::forward<Args>(args)...);
        V& ref = *v;
        register_venue(std::move(v));
        return ref;
    }

    // Register an instrument tradable on a venue. price_sig_figs: see
    // Instrument (Hyperliquid: 5; 0 = no limit).
    const Instrument& add_instrument(IVenue& venue, std::string symbol, double tick_size,
                                     double lot_size, double min_qty, int price_sig_figs = 0);

    const Instrument* find_instrument(std::string_view venue_name, std::string_view symbol) const;
    const std::deque<Instrument>& instruments() const { return instruments_; }

    // Keep the instrument's book up to date even without an algorithm on it
    // (e.g. to show prices). Engine thread. Does nothing if already followed.
    void watch_book(const Instrument& inst);

    // Create and start an algorithm. A's constructor receives (AlgoContext, args...).
    // Automatically subscribe to the instrument's order book.
    template <class A, class... Args>
    A& start_algo(IVenue& venue, const Instrument& inst, Args&&... args) {
        auto a = std::make_unique<A>(make_context(venue, inst), std::forward<Args>(args)...);
        A& ref = *a;
        register_algo(std::move(a), venue, inst);
        return ref;
    }

    // Connect all venues and subscribe to private streams.
    void connect_all();

    // Disconnect every venue, waiting for their network threads to stop.
    // After this, no venue writes to the queues anymore. Idempotent.
    void shutdown();

    // One loop iteration: timers, request timeouts, then up to
    // max_events_per_venue events from each venue.
    // Return the number of processed events. Used by tests.
    size_t poll();

    // Main loop: continue while algorithms are active or until
    // stop() is called.
    void run();
    void stop() { running_ = false; }

    // Ask all algorithms to stop. Must be called from the engine thread.
    void stop_algos();

    // Thread-safe and async-signal-safe (only stores an atomic flag): the
    // engine calls stop_algos() itself at the start of the next poll().
    // Use this from signal handlers (Ctrl+C) and other threads.
    void request_stop_algos() noexcept { stop_algos_requested_.store(true, std::memory_order_relaxed); }

    bool has_active_algos() const;

    // Interval between algorithm on_timer() calls (0 = never).
    void set_timer_interval(Duration d) { opt_.timer_interval = d; }

    OrderManager& oms() { return oms_; }
    const OrderBook& book(InstrumentId inst) const { return books_.at(inst); }
    const IClock& clock() const { return *clock_; }

private:
    // A venue with its event queue. The venue (network thread) writes
    // to the queue; the engine reads.
    struct VenueSlot final : IEventSink {
        explicit VenueSlot(size_t capacity) : queue(capacity) {}
        void on_event(Event&& e) override;

        // Declared BEFORE `venue`, so it is destroyed AFTER it: the venue
        // can never write into an already destroyed queue.
        SpscQueue<Event> queue;
        // Set by the producer when an event could not be queued. From then
        // on, events are dropped until the engine has resynchronized.
        std::atomic<bool> overflowed{false};
        std::atomic<std::thread::id> producer{};  // Checks the single-producer contract.
        std::unique_ptr<IVenue> venue;
    };

    struct AlgoSlot {
        std::unique_ptr<Algo> algo;
        VenueId venue;
    };

    void init();
    AlgoContext make_context(IVenue& venue, const Instrument& inst);
    void register_venue(std::unique_ptr<IVenue> v);
    void register_algo(std::unique_ptr<Algo> a, IVenue& venue, const Instrument& inst);
    void run_timers();
    void handle_overflow(VenueSlot& slot);
    void dispatch(Event& e);

    std::unique_ptr<IClock> owned_clock_;
    const IClock* clock_;
    Options opt_;

    OrderManager oms_;
    std::vector<std::unique_ptr<VenueSlot>> venues_;
    // deque: algorithms hold references to instruments and books, so existing
    // elements must not move in memory when new ones are added.
    std::deque<Instrument> instruments_;
    std::deque<OrderBook> books_;
    std::vector<std::vector<AlgoId>> book_subscribers_;  // Index: InstrumentId.
    std::vector<bool> book_subscribed_;                  // Index: InstrumentId.
    std::vector<AlgoSlot> algos_;                        // Index: AlgoId.

    Timestamp last_timer_{};
    bool shut_down_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_algos_requested_{false};
    static_assert(std::atomic<bool>::is_always_lock_free, "needed for async-signal safety");
};

}  // namespace peewee
