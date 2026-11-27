#include "peewee/engine/engine.hpp"

#include <cstdlib>

#include "peewee/core/log.hpp"

namespace peewee {

namespace {
constexpr int kDefaultBookDepth = 50;

// Live processes start ClientOrderIds at "now in ms" x 1000: every run uses
// IDs above the previous run's, as exchanges reject a reused client ID.
ClientOrderId first_live_cid(const IClock& clock) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock.wall()).count();
    return static_cast<ClientOrderId>(ms) * 1000;
}
}  // namespace

void Engine::VenueSlot::on_event(Event&& e) {
    // Enforce the single-producer contract: a second thread writing into
    // this queue would corrupt it silently, so fail loudly instead.
    const std::thread::id me = std::this_thread::get_id();
    std::thread::id first{};
    if (!producer.compare_exchange_strong(first, me, std::memory_order_relaxed) && first != me) {
        PEEWEE_LOG("venue %s: on_event called from two threads; all connections of a venue "
                   "must share one network thread", std::string(venue->name()).c_str());
        std::abort();
    }

    if (overflowed.load(std::memory_order_relaxed)) return;  // Gap already open: wait for resync.
    if (!queue.try_push(std::move(e))) {
        // The engine cannot keep up. Do not log here (this is the network
        // thread and logging may block): the engine notices the flag.
        overflowed.store(true, std::memory_order_release);
    }
}

Engine::Engine(Options opt)
    : owned_clock_(std::make_unique<RealClock>()),
      clock_(owned_clock_.get()),
      opt_(opt),
      oms_(*clock_, {.request_timeout = opt.request_timeout, .first_cid = first_live_cid(*clock_)}) {
    init();
}

// External clock (tests, simulation): IDs start at 1, reproducible runs.
Engine::Engine(const IClock& clock, Options opt)
    : clock_(&clock), opt_(opt), oms_(clock, {.request_timeout = opt.request_timeout}) {
    init();
}

void Engine::init() {
    oms_.set_listener([this](const Order& o, const OrderEvent& e) {
        if (o.owner < algos_.size()) {
            Algo& a = *algos_[o.owner].algo;
            if (!a.done()) a.on_order(o, e);
        }
    });
}

Engine::~Engine() {
    shutdown();
}

void Engine::shutdown() {
    if (shut_down_) return;
    shut_down_ = true;
    for (auto& s : venues_) s->venue->disconnect();
}

void Engine::register_venue(std::unique_ptr<IVenue> v) {
    auto slot = std::make_unique<VenueSlot>(opt_.queue_capacity);
    slot->venue = std::move(v);
    slot->venue->set_sink(slot.get());
    oms_.add_venue(*slot->venue);
    venues_.push_back(std::move(slot));
}

const Instrument& Engine::add_instrument(IVenue& venue, std::string symbol, double tick_size,
                                         double lot_size, double min_qty, int price_sig_figs) {
    Instrument inst;
    inst.price_sig_figs = price_sig_figs;
    inst.id = static_cast<InstrumentId>(instruments_.size());
    inst.venue = venue.id();
    inst.symbol = std::move(symbol);
    inst.tick_size = tick_size;
    inst.lot_size = lot_size;
    inst.min_qty = inst.qty(min_qty);

    instruments_.push_back(inst);
    books_.emplace_back(inst.id);
    book_subscribers_.emplace_back();
    book_subscribed_.push_back(false);

    venue.add_instrument(instruments_.back());
    return instruments_.back();
}

const Instrument* Engine::find_instrument(std::string_view venue_name, std::string_view symbol) const {
    for (const Instrument& i : instruments_) {
        if (i.symbol == symbol && venues_.at(i.venue)->venue->name() == venue_name) return &i;
    }
    return nullptr;
}

AlgoContext Engine::make_context(IVenue& venue, const Instrument& inst) {
    return AlgoContext{oms_, venue, *clock_, inst, books_.at(inst.id)};
}

void Engine::register_algo(std::unique_ptr<Algo> a, IVenue& venue, const Instrument& inst) {
    const auto id = static_cast<AlgoId>(algos_.size());
    a->id_ = id;
    algos_.push_back(AlgoSlot{std::move(a), venue.id()});
    book_subscribers_.at(inst.id).push_back(id);

    if (!book_subscribed_.at(inst.id)) {
        book_subscribed_[inst.id] = true;
        venue.subscribe_book(inst.id, kDefaultBookDepth);
    }

    algos_.back().algo->on_start();
}

void Engine::connect_all() {
    for (auto& s : venues_) {
        s->venue->connect();
        s->venue->subscribe_private();
    }
}

size_t Engine::poll() {
    if (stop_algos_requested_.exchange(false, std::memory_order_relaxed)) stop_algos();

    // Timers before events, so responses to requests sent from on_timer()
    // are collected in this iteration.
    run_timers();
    oms_.check_timeouts();

    for (auto& s : venues_) s->venue->poll();

    size_t n = 0;
    Event e;
    for (auto& s : venues_) {
        if (s->overflowed.load(std::memory_order_acquire)) {
            // Everything still queued precedes the gap and is valid: process
            // all of it (the producer is not adding more), then resync.
            while (s->queue.try_pop(e)) {
                dispatch(e);
                ++n;
            }
            handle_overflow(*s);
            continue;
        }
        for (size_t k = 0; k < opt_.max_events_per_venue && s->queue.try_pop(e); ++k) {
            dispatch(e);
            ++n;
        }
    }
    return n;
}

void Engine::run_timers() {
    if (opt_.timer_interval <= Duration::zero()) return;
    const Timestamp t = clock_->now();
    if (t - last_timer_ < opt_.timer_interval) return;
    last_timer_ = t;
    for (auto& slot : algos_) {
        if (!slot.algo->done()) slot.algo->on_timer();
    }
}

void Engine::handle_overflow(VenueSlot& slot) {
    const VenueId v = slot.venue->id();
    PEEWEE_LOG("venue %s: event queue overflowed, events were lost; resynchronizing",
               std::string(slot.venue->name()).c_str());

    // Treat it as a disconnection of both streams: books become invalid and
    // algorithms are told. The venue then sends fresh snapshots and
    // VenueStatus{up}, and Private-up triggers order reconciliation.
    Event down_public = VenueStatus{v, StreamKind::Public, false, clock_->wall()};
    Event down_private = VenueStatus{v, StreamKind::Private, false, clock_->wall()};
    dispatch(down_public);
    dispatch(down_private);

    slot.overflowed.store(false, std::memory_order_release);
    slot.venue->resync();
}

void Engine::dispatch(Event& e) {
    std::visit(overloaded{
        [&](BookDelta& d) {
            if (d.inst >= books_.size()) return;
            OrderBook& book = books_[d.inst];
            if (!book.apply(d)) return;
            for (AlgoId id : book_subscribers_[d.inst]) {
                Algo& a = *algos_[id].algo;
                if (!a.done()) a.on_book(book);
            }
        },
        [&](OrderEvent& oe) {
            oms_.on_event(oe);  // The listener installed in init() notifies the algorithm.
        },
        [&](VenueStatus& st) {
            if (!st.up && st.stream == StreamKind::Public) {
                for (OrderBook& b : books_) {
                    if (instruments_[b.instrument()].venue == st.venue) b.invalidate();
                }
            }
            // While the private stream was down, fills and closes may have
            // been missed: ask the exchange for the real state of every order.
            if (st.up && st.stream == StreamKind::Private) oms_.reconcile_venue(st.venue);

            for (auto& slot : algos_) {
                if (slot.venue == st.venue && !slot.algo->done()) slot.algo->on_venue_status(st);
            }
        },
    }, e);
}

void Engine::run() {
    running_ = true;
    while (running_ && has_active_algos()) {
        // Busy polling minimizes latency but uses an entire core. Alternatively,
        // wait on a condition variable when there are no events.
        if (poll() == 0) std::this_thread::yield();
    }
}

void Engine::stop_algos() {
    for (auto& slot : algos_) {
        if (!slot.algo->done()) slot.algo->stop();
    }
}

bool Engine::has_active_algos() const {
    for (const auto& slot : algos_) {
        if (!slot.algo->done()) return true;
    }
    return false;
}

}  // namespace peewee
