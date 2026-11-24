#include "peewee/venues/sim.hpp"

#include <algorithm>

namespace peewee {

SimVenue::SimVenue(VenueId id, const IClock& clock, SimConfig cfg)
    : id_(id), clock_(clock), cfg_(std::move(cfg)) {
    if (cfg_.max_requests_per_sec > 0) limiter_.emplace(cfg_.max_requests_per_sec, cfg_.request_burst);
}

void SimVenue::add_instrument(const Instrument& inst) {
    books_.try_emplace(inst.id, inst.id);
}

void SimVenue::subscribe_book(InstrumentId inst, int /*depth*/) {
    subscribed_[inst] = true;
    publish_book(inst);
}

bool SimVenue::can_send(RequestKind /*kind*/) const {
    return !limiter_ || limiter_->available(now());
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

bool SimVenue::check_reject(RequestKind kind, ClientOrderId cid, RequestId req) {
    if (limiter_ && !limiter_->try_acquire(now())) {
        respond(kind, OrderReject{cid, req, kind, RejectReason::RateLimited, "sim: rate limit", ts()});
        return true;
    }
    auto& forced = reject_next_[static_cast<size_t>(kind)];
    if (forced) {
        respond(kind, OrderReject{cid, req, kind, *forced, "sim: forced reject", ts()});
        forced.reset();
        return true;
    }
    return false;
}

void SimVenue::send_new(const NewOrder& n) {
    requests_.push_back({RequestKind::New, n.cid, n.px, now()});
    if (check_reject(RequestKind::New, n.cid, n.req)) return;

    SimOrder o;
    o.cid = n.cid;
    o.inst = n.inst;
    o.side = n.side;
    o.type = n.type;
    o.tif = n.tif;
    o.px = n.px;
    o.qty = n.qty;
    o.trigger_px = n.trigger_px;
    o.open = true;
    o.waiting_trigger = is_trigger(n.type);

    if (!o.waiting_trigger && n.type == OrderType::Limit && crosses(o, o.px) && o.tif == Tif::PostOnly) {
        respond(RequestKind::New, OrderReject{n.cid, n.req, RequestKind::New, RejectReason::PostOnlyWouldCross,
                                              "sim: post-only would cross", ts()});
        return;
    }

    SimOrder& stored = orders_[o.cid] = o;
    respond(RequestKind::New, OrderAck{o.cid, n.req, "sim-" + std::to_string(o.cid), o.px, o.qty, ts()});

    if (stored.waiting_trigger) {
        // The condition may already be true (like on the real exchanges).
        if (const auto m = mid(o.inst)) check_triggers(o.inst, *m);
        return;
    }
    enter_book(stored);
    publish_book(o.inst);
}

void SimVenue::enter_book(SimOrder& o) {
    const bool may_rest = !is_market(o.type) && o.tif != Tif::IOC && o.tif != Tif::FOK;
    if (crosses(o, o.px)) {
        fill_as_taker(o);
    } else if (!may_rest) {
        o.open = false;
        emit_private(OrderClosed{o.cid, kNoRequest, CloseReason::Canceled, ts()});
    }
}

void SimVenue::check_triggers(InstrumentId inst, Price mark) {
    bool changed = false;
    for (auto& [cid, o] : orders_) {
        if (!o.open || !o.waiting_trigger || o.inst != inst) continue;
        const bool hit = triggers_on_rise(o.type, o.side == Side::Buy) ? mark >= o.trigger_px
                                                                        : mark <= o.trigger_px;
        if (!hit) continue;
        o.waiting_trigger = false;
        enter_book(o);
        changed = true;
    }
    if (changed) publish_book(inst);
}

void SimVenue::close_after_amend_reject(ClientOrderId cid) {
    if (!cfg_.amend_reject_cancels) return;
    auto it = orders_.find(cid);
    if (it == orders_.end() || !it->second.open) return;
    it->second.open = false;
    emit_private(OrderClosed{cid, kNoRequest, CloseReason::Canceled, ts()});
    publish_book(it->second.inst);
}

std::optional<Price> SimVenue::mid(InstrumentId inst) const {
    const OrderBook& b = books_.at(inst);
    const auto bid = b.best_bid();
    const auto ask = b.best_ask();
    if (!bid || !ask) return std::nullopt;
    return Price{(bid->ticks + ask->ticks) / 2};
}

void SimVenue::send_amend(const AmendOrder& a) {
    requests_.push_back({RequestKind::Amend, a.cid, a.px, now()});
    if (check_reject(RequestKind::Amend, a.cid, a.req)) {
        close_after_amend_reject(a.cid);
        return;
    }

    auto it = orders_.find(a.cid);
    if (it == orders_.end() || !it->second.open) {
        respond(RequestKind::Amend, OrderReject{a.cid, a.req, RequestKind::Amend, RejectReason::OrderNotFound,
                                                "sim: order not found", ts()});
        return;
    }
    SimOrder& o = it->second;

    if (o.waiting_trigger) {
        // Not in the book yet: only the price it will use changes.
        o.px = a.px;
        if (a.qty) o.qty = *a.qty;
        respond(RequestKind::Amend, AmendAck{o.cid, a.req, o.px, o.qty, ts()});
        return;
    }

    if (o.type == OrderType::Limit && crosses(o, a.px) && o.tif == Tif::PostOnly) {
        // Reject the amend; the order stays at its previous price (unless
        // amend_reject_cancels: then it is gone, like on Hyperliquid).
        respond(RequestKind::Amend, OrderReject{a.cid, a.req, RequestKind::Amend, RejectReason::PostOnlyWouldCross,
                                                "sim: post-only would cross", ts()});
        close_after_amend_reject(a.cid);
        return;
    }

    o.px = a.px;
    if (a.qty) o.qty = *a.qty;
    respond(RequestKind::Amend, AmendAck{o.cid, a.req, o.px, o.qty, ts()});
    if (crosses(o, o.px)) fill_as_taker(o);
    publish_book(o.inst);
}

void SimVenue::send_cancel(const CancelOrder& c) {
    requests_.push_back({RequestKind::Cancel, c.cid, Price{}, now()});
    if (check_reject(RequestKind::Cancel, c.cid, c.req)) return;

    auto it = orders_.find(c.cid);
    if (it == orders_.end() || !it->second.open) {
        respond(RequestKind::Cancel, OrderReject{c.cid, c.req, RequestKind::Cancel, RejectReason::OrderNotFound,
                                                 "sim: order not found", ts()});
        return;
    }
    it->second.open = false;
    respond(RequestKind::Cancel, OrderClosed{c.cid, c.req, CloseReason::Canceled, ts()});
    publish_book(it->second.inst);
}

void SimVenue::query_order(const QueryOrder& q) {
    ++queries_;
    OrderStatus s{.cid = q.cid, .found = false, .open = false, .px = {}, .qty = {}, .filled = {}, .ts = ts()};
    if (auto it = orders_.find(q.cid); it != orders_.end()) {
        const SimOrder& o = it->second;
        s.found = true;
        s.open = o.open;
        s.px = o.px;
        s.qty = o.qty;
        s.filled = o.filled;
    }
    emit(OrderEvent{s});  // Queries are answered over REST: always delivered.
}

void SimVenue::resync() {
    public_up_ = true;
    private_up_ = true;
    for (auto& [inst, sub] : subscribed_) {
        if (sub) publish_book(inst);
    }
    emit(VenueStatus{id_, StreamKind::Public, true, ts()});
    emit(VenueStatus{id_, StreamKind::Private, true, ts()});
}

bool SimVenue::crosses(const SimOrder& o, Price px) const {
    const OrderBook& b = books_.at(o.inst);
    if (o.side == Side::Buy) {
        auto ask = b.best_ask();
        return ask && px >= *ask;
    }
    auto bid = b.best_bid();
    return bid && px <= *bid;
}

void SimVenue::fill_as_taker(SimOrder& o) {
    const OrderBook& b = books_.at(o.inst);
    const Price px = o.side == Side::Buy ? *b.best_ask() : *b.best_bid();
    const Qty q = o.qty - o.filled;
    o.filled = o.qty;
    o.open = false;
    emit_fill(o, px, q, /*maker=*/false);
    emit_private(OrderClosed{o.cid, kNoRequest, CloseReason::Filled, ts()});
}

// ---------------------------------------------------------------------------
// Test controls
// ---------------------------------------------------------------------------

void SimVenue::set_book(InstrumentId inst, std::vector<Level> bids, std::vector<Level> asks) {
    OrderBook& b = books_.at(inst);
    BookDelta d{.inst = inst, .seq = b.seq() + 1, .snapshot = true, .ts = ts(),
                .bids = std::move(bids), .asks = std::move(asks)};
    b.apply(d);
    publish_book(inst);
    if (const auto m = mid(inst)) check_triggers(inst, *m);
}

void SimVenue::update_book(InstrumentId inst, std::vector<Level> bids, std::vector<Level> asks) {
    OrderBook& b = books_.at(inst);
    BookDelta d{.inst = inst, .seq = b.seq() + 1, .snapshot = false, .ts = ts(),
                .bids = std::move(bids), .asks = std::move(asks)};
    b.apply(d);
    publish_book(inst);
    if (const auto m = mid(inst)) check_triggers(inst, *m);
}

void SimVenue::trade(InstrumentId inst, Side aggressor, Price px, Qty qty) {
    const Side passive = opposite(aggressor);

    // Our orders hit by this trade, sorted by price priority.
    std::vector<SimOrder*> hit;
    for (auto& [cid, o] : orders_) {
        if (!o.open || o.waiting_trigger || o.inst != inst || o.side != passive) continue;
        if (passive == Side::Buy ? o.px >= px : o.px <= px) hit.push_back(&o);
    }
    std::sort(hit.begin(), hit.end(), [passive](const SimOrder* a, const SimOrder* b) {
        return passive == Side::Buy ? a->px > b->px : a->px < b->px;
    });

    for (SimOrder* o : hit) {
        if (qty.is_zero()) break;
        const Qty q = std::min(qty, o->qty - o->filled);
        o->filled += q;
        qty -= q;
        emit_fill(*o, o->px, q, /*maker=*/true);
        if (o->filled >= o->qty) {
            o->open = false;
            emit_private(OrderClosed{o->cid, kNoRequest, CloseReason::Filled, ts()});
        }
    }
    publish_book(inst);
    check_triggers(inst, px);  // The last trade is the sim's mark price.
}

void SimVenue::reject_next(RequestKind kind, RejectReason reason) {
    reject_next_[static_cast<size_t>(kind)] = reason;
}

void SimVenue::set_public_stream(bool up) {
    public_up_ = up;
    emit(VenueStatus{id_, StreamKind::Public, up, ts()});
    if (up) {
        for (auto& [inst, sub] : subscribed_) {
            if (sub) publish_book(inst);
        }
    }
}

void SimVenue::drop_next_response(RequestKind kind) {
    drop_next_response_[static_cast<size_t>(kind)] = true;
}

void SimVenue::set_private_stream(bool up) {
    private_up_ = up;
    emit(VenueStatus{id_, StreamKind::Private, up, ts()});
}

void SimVenue::replay_last_fill() {
    if (last_fill_) emit(OrderEvent{*last_fill_});
}

size_t SimVenue::count_requests(RequestKind kind) const {
    return static_cast<size_t>(std::count_if(requests_.begin(), requests_.end(),
                                             [kind](const Request& r) { return r.kind == kind; }));
}

const SimVenue::SimOrder* SimVenue::order(ClientOrderId cid) const {
    auto it = orders_.find(cid);
    return it == orders_.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// Event publication
// ---------------------------------------------------------------------------

void SimVenue::emit(Event e) {
    outbox_.push_back(Pending{now() + cfg_.latency, std::move(e)});
}

void SimVenue::respond(RequestKind kind, OrderEvent e) {
    bool& drop = drop_next_response_[static_cast<size_t>(kind)];
    if (drop) {
        drop = false;
        return;
    }
    emit(std::move(e));
}

void SimVenue::emit_private(OrderEvent e) {
    if (private_up_) emit(std::move(e));
}

void SimVenue::emit_fill(const SimOrder& o, Price px, Qty q, bool maker) {
    Fill f{o.cid, "sim-exec-" + std::to_string(next_exec_++), px, q, maker, ts()};
    last_fill_ = f;
    emit_private(std::move(f));
}

void SimVenue::poll() {
    while (!outbox_.empty() && outbox_.front().at <= now()) {
        if (sink_) sink_->on_event(std::move(outbox_.front().ev));
        outbox_.pop_front();
    }
}

void SimVenue::publish_book(InstrumentId inst) {
    if (!public_up_ || !subscribed_[inst]) return;
    const OrderBook& b = books_.at(inst);
    if (!b.valid()) return;

    BookDelta d{.inst = inst, .seq = ++seq_[inst], .snapshot = true, .ts = ts(), .bids = {}, .asks = {}};
    for (size_t i = 0; i < b.depth(Side::Buy); ++i) d.bids.push_back(b.level(Side::Buy, i));
    for (size_t i = 0; i < b.depth(Side::Sell); ++i) d.asks.push_back(b.level(Side::Sell, i));

    // Our orders also appear in the public book, as on a real exchange.
    auto add = [](std::vector<Level>& lv, Level l, bool descending) {
        auto it = std::find_if(lv.begin(), lv.end(), [&](const Level& x) {
            return descending ? x.px <= l.px : x.px >= l.px;
        });
        if (it != lv.end() && it->px == l.px) it->qty += l.qty;
        else lv.insert(it, l);
    };
    for (const auto& [cid, o] : orders_) {
        if (!o.open || o.waiting_trigger || o.inst != inst) continue;
        const Level l{o.px, o.qty - o.filled};
        if (o.side == Side::Buy) add(d.bids, l, true);
        else add(d.asks, l, false);
    }
    emit(std::move(d));
}

}  // namespace peewee
