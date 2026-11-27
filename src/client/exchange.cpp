#include "peewee/client/exchange.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <variant>

#include "peewee/algo/chase_order.hpp"
#include "peewee/engine/engine.hpp"
#include "peewee/venues/bybit.hpp"
#include "peewee/venues/hyperliquid.hpp"
#include "peewee/venues/lighter.hpp"

namespace peewee {

// ---------------------------------------------------------------------------
// OrderHandle: written by the engine thread, read by any thread.
// ---------------------------------------------------------------------------

struct OrderHandle::Shared {
    mutable std::mutex m;
    mutable std::condition_variable cv;
    State state = State::Working;
    double filled = 0.0;
    double avg_price = 0.0;
    std::string error;
    std::weak_ptr<Exchange::Impl> exchange;  // For cancel().
};

OrderHandle::State OrderHandle::state() const {
    std::lock_guard lock(s_->m);
    return s_->state;
}

const char* OrderHandle::state_text() const {
    switch (state()) {
        case State::Working:  return "working";
        case State::Filled:   return "filled";
        case State::Canceled: return "canceled";
        case State::Rejected: return "rejected";
        case State::Expired:  return "expired";
    }
    return "?";
}

bool OrderHandle::done() const { return state() != State::Working; }

double OrderHandle::filled() const {
    std::lock_guard lock(s_->m);
    return s_->filled;
}

double OrderHandle::avg_price() const {
    std::lock_guard lock(s_->m);
    return s_->avg_price;
}

std::string OrderHandle::error() const {
    std::lock_guard lock(s_->m);
    return s_->error;
}

void OrderHandle::wait() const {
    std::unique_lock lock(s_->m);
    s_->cv.wait(lock, [this] { return s_->state != State::Working; });
}

bool OrderHandle::wait_for(std::chrono::milliseconds max) const {
    std::unique_lock lock(s_->m);
    return s_->cv.wait_for(lock, max, [this] { return s_->state != State::Working; });
}

// ---------------------------------------------------------------------------
// Exchange internals
// ---------------------------------------------------------------------------

namespace {

constexpr auto kCloseTimeout = std::chrono::seconds(10);

// One limit or market order, run as an algorithm so that it goes through the
// OMS like everything else (one request at a time, timeouts, queries).
class SingleOrder final : public Algo {
public:
    SingleOrder(AlgoContext ctx, NewOrder order, double max_move_pct)
        : Algo(ctx), order_(order), max_move_pct_(max_move_pct) {}

    void on_start() override {
        if (order_.type != OrderType::Market) send();
    }

    void on_book(const OrderBook& book) override {
        // A market order takes its worst price from the book: wait for one.
        if (cid_ != kNoOrder || done_ || stop_requested_ || !book.valid()) return;
        const std::optional<Price> ref = order_.side == Side::Buy ? book.best_ask() : book.best_bid();
        if (!ref) return;
        const auto move = static_cast<int64_t>(std::llround(ref->ticks * max_move_pct_ / 100.0));
        // Rounded to a valid price on the safe side: never beyond max_move_pct.
        const Instrument& inst = ctx_.instrument;
        order_.px = order_.side == Side::Buy ? inst.round_down(*ref + move) : inst.round_up(*ref - move);
        send();
    }

    void on_order(const Order& order, const OrderEvent& event) override {
        if (const auto* f = std::get_if<Fill>(&event)) {
            fill_value_ += f->px.ticks * f->qty.lots;
            fill_lots_ += f->qty.lots;
        }
        if (const auto* r = std::get_if<OrderReject>(&event); r && r->request == RequestKind::New) {
            error_ = r->text;
        }
        filled_ = order.state == OrderState::Filled ? order.qty : order.filled;
        if (is_terminal(order.state)) {
            done_ = true;
            final_ = order.state;
        }
    }

    void on_timer() override {
        // A cancel cannot leave while another request is pending: retry.
        if (stop_requested_ && !done_) cancel_now();
    }

    void stop() override {
        stop_requested_ = true;
        if (cid_ == kNoOrder) {  // Market order still waiting for a book.
            done_ = true;
            final_ = OrderState::Canceled;
            return;
        }
        cancel_now();
    }

    bool done() const override { return done_; }

    OrderState final_state() const { return final_; }
    Qty filled() const { return filled_; }
    double average_fill_ticks() const {
        return fill_lots_ > 0 ? static_cast<double>(fill_value_) / static_cast<double>(fill_lots_) : 0.0;
    }
    const std::string& error() const { return error_; }

private:
    void send() { cid_ = ctx_.oms.send_new(id(), ctx_.venue, order_); }

    void cancel_now() {
        const Order* o = ctx_.oms.find(cid_);
        if (o && !is_terminal(o->state) && !o->has_pending()) ctx_.oms.send_cancel(cid_);
    }

    NewOrder order_;
    double max_move_pct_;
    ClientOrderId cid_ = kNoOrder;
    bool done_ = false;
    bool stop_requested_ = false;
    OrderState final_ = OrderState::PendingNew;
    Qty filled_;
    int64_t fill_value_ = 0;
    int64_t fill_lots_ = 0;
    std::string error_;
};

OrderHandle::State from_chase(ChaseOrder::Result r) {
    switch (r) {
        case ChaseOrder::Result::Filled:   return OrderHandle::State::Filled;
        case ChaseOrder::Result::Rejected: return OrderHandle::State::Rejected;
        case ChaseOrder::Result::Expired:  return OrderHandle::State::Expired;
        default:                           return OrderHandle::State::Canceled;
    }
}

OrderHandle::State from_order(OrderState s) {
    switch (s) {
        case OrderState::Filled:   return OrderHandle::State::Filled;
        case OrderState::Rejected: return OrderHandle::State::Rejected;
        default:                   return OrderHandle::State::Canceled;
    }
}

}  // namespace

struct Exchange::Impl {
    // An order being worked, seen from the engine thread.
    struct Tracked {
        Algo* algo;
        std::shared_ptr<OrderHandle::Shared> s;
        std::function<void()> update;  // Copies the algorithm's progress into s.
    };

    std::unique_ptr<Engine> engine;
    IVenue* venue = nullptr;
    std::thread thread;

    std::mutex m;                                  // Guards `commands`.
    std::deque<std::function<void()>> commands;    // From any thread, run on the engine thread.

    // Engine thread only.
    std::vector<Tracked> active;
    bool closing = false;
    std::chrono::steady_clock::time_point close_deadline;

    void post(std::function<void()> fn) {
        std::lock_guard lock(m);
        commands.push_back(std::move(fn));
    }

    const Instrument& instrument(std::string_view symbol) const {
        // Instruments are all added before the engine thread starts: reading
        // them from another thread is safe.
        const Instrument* inst = engine->find_instrument(venue->name(), symbol);
        if (!inst) throw std::invalid_argument("peewee: unknown symbol '" + std::string(symbol) + "'");
        return *inst;
    }

    void stop_all() {
        for (auto& t : active) {
            if (!t.algo->done()) t.algo->stop();
        }
    }

    void loop() {
        for (;;) {
            std::deque<std::function<void()>> todo;
            {
                std::lock_guard lock(m);
                todo.swap(commands);
            }
            for (auto& fn : todo) fn();

            const size_t events = engine->poll();

            for (auto& t : active) t.update();
            active.erase(std::remove_if(active.begin(), active.end(),
                                        [](const Tracked& t) { return t.algo->done(); }),
                         active.end());

            if (closing && (active.empty() || std::chrono::steady_clock::now() > close_deadline)) break;
            if (events == 0 && todo.empty()) std::this_thread::sleep_for(std::chrono::microseconds(200));
        }

        // Orders whose cancel was not confirmed in time: never leave a wait() hanging.
        for (auto& t : active) {
            std::lock_guard lock(t.s->m);
            t.s->state = OrderHandle::State::Canceled;
            t.s->error = "closed before the exchange confirmed the cancel: check the order on the exchange";
            t.s->cv.notify_all();
        }
        active.clear();
    }

    // Engine thread: start tracking an algorithm for a handle.
    void track(Algo* algo, std::shared_ptr<OrderHandle::Shared> s, std::function<void()> update) {
        active.push_back(Tracked{algo, std::move(s), std::move(update)});
        active.back().update();
    }
};

// Called on the engine thread after each poll: publish progress, and the
// final state once the algorithm is done.
static void publish(OrderHandle::Shared& s, double filled, double avg_price, bool done, OrderHandle::State state,
                    const std::string& error) {
    std::lock_guard lock(s.m);
    s.filled = filled;
    s.avg_price = avg_price;
    if (done && s.state == OrderHandle::State::Working) {
        s.state = state;
        s.error = error;
        s.cv.notify_all();
    }
}

void OrderHandle::cancel() {
    std::shared_ptr<Exchange::Impl> ex;
    {
        std::lock_guard lock(s_->m);
        if (s_->state != State::Working) return;
        ex = s_->exchange.lock();
    }
    if (!ex) return;
    ex->post([impl = ex.get(), s = s_] {
        for (auto& t : impl->active) {
            if (t.s == s && !t.algo->done()) t.algo->stop();
        }
    });
}

// ---------------------------------------------------------------------------
// Exchange
// ---------------------------------------------------------------------------

Exchange::Exchange(std::unique_ptr<Engine> engine, IVenue& venue) : impl_(std::make_shared<Impl>()) {
    impl_->engine = std::move(engine);
    impl_->venue = &venue;
    impl_->thread = std::thread([impl = impl_.get()] { impl->loop(); });
}

Exchange::Exchange(Exchange&&) noexcept = default;
Exchange& Exchange::operator=(Exchange&&) noexcept = default;

Exchange::~Exchange() {
    if (!impl_) return;  // Moved from.
    // Cancel what is still working, then let the loop end (at most kCloseTimeout).
    impl_->post([impl = impl_.get()] {
        impl->stop_all();
        impl->closing = true;
        impl->close_deadline = std::chrono::steady_clock::now() + kCloseTimeout;
    });
    impl_->thread.join();
    impl_->engine->shutdown();
}

Exchange Exchange::hyperliquid(const std::vector<std::string>& coins, bool testnet) {
    const char* account = std::getenv("HL_ACCOUNT");
    const char* key = std::getenv("HL_API_KEY");
    if (!account || !key) throw std::runtime_error("peewee: set HL_ACCOUNT and HL_API_KEY");

    auto engine = std::make_unique<Engine>();
    HyperliquidConfig cfg;
    cfg.account_address = account;
    cfg.api_wallet_key = key;
    cfg.testnet = testnet;
    auto& venue = engine->add_venue<HyperliquidVenue>(cfg);
    for (const auto& coin : coins) {
        const auto spec = venue.fetch_instrument(coin);
        engine->add_instrument(venue, coin, spec.tick_size, spec.lot_size, spec.min_qty, spec.price_sig_figs);
    }
    engine->connect_all();
    return Exchange(std::move(engine), venue);
}

Exchange Exchange::bybit(const std::vector<std::string>& symbols, bool testnet) {
    const char* key = std::getenv("BYBIT_API_KEY");
    const char* secret = std::getenv("BYBIT_API_SECRET");
    if (!key || !secret) throw std::runtime_error("peewee: set BYBIT_API_KEY and BYBIT_API_SECRET");

    auto engine = std::make_unique<Engine>();
    BybitConfig cfg;
    cfg.api_key = key;
    cfg.api_secret = secret;
    cfg.testnet = testnet;
    auto& venue = engine->add_venue<BybitVenue>(cfg);
    for (const auto& symbol : symbols) {
        const auto spec = venue.fetch_instrument(symbol);
        engine->add_instrument(venue, symbol, spec.tick_size, spec.lot_size, spec.min_qty);
    }
    engine->connect_all();
    return Exchange(std::move(engine), venue);
}

Exchange Exchange::lighter(const std::vector<std::string>& symbols, bool testnet) {
    const char* account = std::getenv("LIGHTER_ACCOUNT_INDEX");
    const char* key_index = std::getenv("LIGHTER_API_KEY_INDEX");
    const char* key = std::getenv("LIGHTER_API_KEY");
    if (!account || !key_index || !key) {
        throw std::runtime_error("peewee: set LIGHTER_ACCOUNT_INDEX, LIGHTER_API_KEY_INDEX and LIGHTER_API_KEY");
    }

    auto engine = std::make_unique<Engine>();
    LighterConfig cfg;
    cfg.account_index = std::stoll(account);
    cfg.api_key_index = std::stoi(key_index);
    cfg.api_private_key = key;
    cfg.testnet = testnet;
    auto& venue = engine->add_venue<LighterVenue>(cfg);
    for (const auto& symbol : symbols) {
        const auto spec = venue.fetch_instrument(symbol);
        engine->add_instrument(venue, symbol, spec.tick_size, spec.lot_size, spec.min_qty);
    }
    engine->connect_all();
    return Exchange(std::move(engine), venue);
}

namespace {

Qty checked_qty(const Instrument& inst, double qty) {
    const Qty q = inst.qty(qty);
    if (q.lots <= 0) {
        throw std::invalid_argument("peewee: quantity " + std::to_string(qty) + " is below one lot of " +
                                    inst.symbol);
    }
    return q;
}

}  // namespace

OrderHandle Exchange::chase(std::string_view symbol, Side side, double qty, ChaseOptions opt) {
    const Instrument& inst = impl_->instrument(symbol);
    ChaseParams p;
    p.side = side;
    p.qty = checked_qty(inst, qty);
    if (opt.limit > 0.0) {
        p.limit = inst.price(opt.limit);
    } else {
        p.max_move_pct = opt.max_move_pct;
    }
    p.min_reprice_interval = opt.reprice_every;
    p.max_reprices = opt.max_reprices;
    p.max_duration = opt.max_duration;
    p.offset_ticks = opt.offset_ticks;

    OrderHandle h;
    h.s_ = std::make_shared<OrderHandle::Shared>();
    h.s_->exchange = impl_;
    impl_->post([impl = impl_.get(), s = h.s_, inst = &inst, p] {
        auto* chase = &impl->engine->start_algo<ChaseOrder>(*impl->venue, *inst, p);
        impl->track(chase, s, [chase, s, inst] {
            publish(*s, inst->to_double(chase->filled()), chase->average_fill_ticks() * inst->tick_size,
                    chase->done(), from_chase(chase->result()), "");
        });
    });
    return h;
}

OrderHandle Exchange::limit(std::string_view symbol, Side side, double qty, double price, LimitOptions opt) {
    const Instrument& inst = impl_->instrument(symbol);
    NewOrder o;
    o.inst = inst.id;
    o.side = side;
    o.type = OrderType::Limit;
    // Rounded to a valid price on the safe side (a buy down, a sell up).
    o.px = side == Side::Buy ? inst.round_down(inst.price(price)) : inst.round_up(inst.price(price));
    o.qty = checked_qty(inst, qty);
    o.tif = opt.tif;
    o.reduce_only = opt.reduce_only;

    OrderHandle h;
    h.s_ = std::make_shared<OrderHandle::Shared>();
    h.s_->exchange = impl_;
    impl_->post([impl = impl_.get(), s = h.s_, inst = &inst, o] {
        auto* algo = &impl->engine->start_algo<SingleOrder>(*impl->venue, *inst, o, 0.0);
        impl->track(algo, s, [algo, s, inst] {
            publish(*s, inst->to_double(algo->filled()), algo->average_fill_ticks() * inst->tick_size,
                    algo->done(), from_order(algo->final_state()), algo->error());
        });
    });
    return h;
}

OrderHandle Exchange::market(std::string_view symbol, Side side, double qty, MarketOptions opt) {
    const Instrument& inst = impl_->instrument(symbol);
    NewOrder o;
    o.inst = inst.id;
    o.side = side;
    o.type = OrderType::Market;
    o.qty = checked_qty(inst, qty);
    o.reduce_only = opt.reduce_only;

    OrderHandle h;
    h.s_ = std::make_shared<OrderHandle::Shared>();
    h.s_->exchange = impl_;
    impl_->post([impl = impl_.get(), s = h.s_, inst = &inst, o, pct = opt.max_move_pct] {
        auto* algo = &impl->engine->start_algo<SingleOrder>(*impl->venue, *inst, o, pct);
        impl->track(algo, s, [algo, s, inst] {
            publish(*s, inst->to_double(algo->filled()), algo->average_fill_ticks() * inst->tick_size,
                    algo->done(), from_order(algo->final_state()), algo->error());
        });
    });
    return h;
}

void Exchange::cancel_all() {
    impl_->post([impl = impl_.get()] { impl->stop_all(); });
}

void Exchange::post(std::function<void(Engine&)> fn) {
    impl_->post([impl = impl_.get(), fn = std::move(fn)] { fn(*impl->engine); });
}

}  // namespace peewee
