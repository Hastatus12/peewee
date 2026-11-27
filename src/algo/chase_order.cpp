// ============================================================================
//  CHASE ORDER: implementation. YOUR TURN.
//
//  All methods are empty: the project builds, but chase does nothing.
//  The questions in these comments guide you; they are not the solution.
//
//  Available tools:
//    ctx_.oms.send_new(id(), ctx_.venue, inst, side, px, qty, tif) -> cid
//    ctx_.oms.send_amend(cid, px)                                   -> bool
//    ctx_.oms.send_cancel(cid)                                      -> bool
//    ctx_.oms.find(cid)                                             -> const Order*
//    ctx_.venue.can_send(RequestKind::Amend)                        -> bool
//    ctx_.venue.caps()                                              -> VenueCapabilities
//    ctx_.book  (the same OrderBook passed to on_book)
//    ctx_.instrument.id
//    now()      (current time from the engine's clock)
//
//  To verify your work:
//    - tests/test_chase_order.cpp: remove SKIP_UNTIL_IMPLEMENTED() one test at a time
//    - examples/chase_sim.cpp:     simulated scenario that prints what happens
// ============================================================================

#include "peewee/algo/chase_order.hpp"

#include <cmath>
#include <optional>

namespace peewee {

ChaseOrder::ChaseOrder(AlgoContext ctx, ChaseParams params) : Algo(ctx), p_(params) {}

void ChaseOrder::on_start() {
    started_at_ = now();
}

void ChaseOrder::on_book(const OrderBook& book) {
    if (!book.valid()) {
        return;
    }

    if (opaque_book_ == true) {
        return;
    }

    if (state_ == State::Init) {
        if (p_.limit.ticks == 0 && p_.max_move_pct > 0.0) {
            // No limit given: allow max_move_pct from the current market.
            // BUY from the best ask, SELL from the best bid: the price we
            // would get right now by crossing the spread.
            const std::optional<Price> ref = p_.side == Side::Buy ? book.best_ask() : book.best_bid();
            if (!ref) {
                return;  // Wait for both sides of the book.
            }
            const auto move = static_cast<int64_t>(std::llround(ref->ticks * p_.max_move_pct / 100.0));
            const Instrument& inst = ctx_.instrument;
            // Rounded to a valid price on the safe side: never beyond max_move_pct.
            p_.limit = p_.side == Side::Buy ? inst.round_down(*ref + move) : inst.round_up(*ref - move);
        }
        Price target = compute_target(book);
        cid_ = ctx_.oms.send_new(id(), ctx_.venue, ctx_.instrument.id,
                                 p_.side,
                                 target,
                                 remaining(),       // After a restart, only what is left.
                                 Tif::PostOnly);
        state_ = State::PendingNew;
    } else if (state_ == State::Working) {
        if (stop_requested_) {
            return; // stopping: on_timer retries the cancel
        }
        if (now() - last_request_at_ < p_.min_reprice_interval) {
            return; // too soon, wake up later
        }
        if (reprices_ >= p_.max_reprices) {
            return;
        }
        Price target = compute_target(book);
        if (target != px_) {
            if (ctx_.oms.send_amend(cid_, target)) {
                state_ = State::PendingAmend;
                last_request_at_ = now();
                ++reprices_;
            }
        }
    }
}

void ChaseOrder::on_order(const Order& order, const OrderEvent& event) {
     std::visit(overloaded{
        [&](const OrderAck& a) {
            px_ = a.px;
            state_ = State::Working;
            on_book(ctx_.book);
        },
        [&](const AmendAck& a) {
            px_ = a.px;
            state_ = State::Working;
            on_book(ctx_.book);
        },
        [&](const Fill& f) {
            filled_ += f.qty;
            fill_value_ += f.px.ticks * f.qty.lots;
            fill_lots_ += f.qty.lots;
            if (remaining().lots <= 0)
                finish(Result::Filled);
        },
        [&](const OrderReject& r) {
            if (r.request != RequestKind::New) {
                // Amend or cancel refused: on Bybit the order is still in the
                // book, at the price the OMS knows. Wait a little, then try
                // again (on_book re-prices, on_timer re-cancels if stopping).
                // On Hyperliquid a refused modify also CANCELS the order: an
                // OrderClosed follows, handled by on_unexpected_close().
                px_ = order.px;
                state_ = State::Working;
                last_request_at_ = now();
                return;
            }
            if (r.reason == RejectReason::PostOnlyWouldCross) {
                state_ = State::Init;            // temporary: retry
            } else {
                finish(Result::Rejected);
            }
        },
        [&](const OrderClosed& c) {
            if (c.reason == CloseReason::Filled) {
                // The fills may arrive after this message: the whole order is done.
                filled_ += order.remaining();
                finish(Result::Filled);
                return;
            }
            on_unexpected_close();
        },
        [&](const OrderStatus& s) {
            // The OMS asked the exchange for the order's real state (after a
            // timeout or a reconnect): trust it, it may reveal missed fills.
            if (s.filled > filled_) {
                filled_ = s.filled;
            }
            if (remaining().lots <= 0) {
                finish(Result::Filled);
            } else if (!s.found || !s.open) {
                on_unexpected_close();
            } else {
                px_ = s.px;
                state_ = State::Working;
            }
        },
        [&](const auto&) {},
    }, event);
}

void ChaseOrder::on_timer() {
    if (now() - started_at_ >= p_.max_duration && p_.max_duration > Duration::zero()) {
        // stop the chase
        expired_ = true;
        stop();
        return;
    }
    if (stop_requested_ && state_ == State::Working) {
        stop(); // the previous cancel was refused: try again
        return;
    }
    on_book(ctx_.book);
}

void ChaseOrder::on_venue_status(const VenueStatus& status) {
    if (status.venue != ctx_.venue.id()) {
        return;
    }

    if (status.stream == StreamKind::Public) {
        opaque_book_ = !status.up;
    }
}

void ChaseOrder::stop() {
    stop_requested_ = true;
    if (state_ == State::Init) {
        finish(Result::Canceled);
        return;
    }
    if (ctx_.oms.send_cancel(cid_)) {
        state_ = State::PendingCancel;
    }
}

bool ChaseOrder::done() const {
    return state_ == State::Done;
}

Price ChaseOrder::compute_target(const OrderBook& book) const {
    std::optional<Price> best_other;

    for (size_t i=0; i < book.depth(p_.side); ++i) {
        const Level& l = book.level(p_.side, i);
        Qty others = l.qty;

        if (l.px == px_) {
            others = others - remaining();
        }
        if (others.lots > 0) {
            best_other = l.px;
            break;
        }
    }
    if (!best_other) {
        return px_;
    }
    
    // "One tick" is one VALID price step: on Hyperliquid it depends on the
    // price (5 significant digits), elsewhere it is simply one tick.
    const Instrument& inst = ctx_.instrument;
    Price target = *best_other;
    for (int i = 0; i < p_.offset_ticks; ++i) {
        target = p_.side == Side::Buy ? inst.step_up(target) : inst.step_down(target);
    }

    // Never retreat: if our order is already ahead of everyone else, keep it
    // there. Moving back would lose queue priority, and when the others come
    // back to our price we would move forward again: 1-tick ping-pong (seen
    // live on Hyperliquid HYPE, 2026-11-22). We still move FORWARD when
    // someone overtakes us. (px_ is zero while no order is in the book.)
    if (px_.ticks > 0) {
        if (p_.side == Side::Buy && target < px_) {
            target = px_;
        }
        if (p_.side == Side::Sell && target > px_) {
            target = px_;
        }
    }

    // Post-only: order must not cross the spread
    if (p_.post_only) {
        if (p_.side == Side::Buy) {
            std::optional<Price> ask = book.best_ask();
            if (ask && target >= *ask) {
                target = inst.step_down(*ask);
            }
        } else {
            std::optional<Price> bid = book.best_bid();
            if (bid && target <= *bid) {
                target = inst.step_up(*bid);
            }
        }
    }

    // If i buy, price must not go above limit
    if (p_.side == Side::Buy && target > p_.limit){
        target = p_.limit;
    }
    // If i sell, price must not go below limit
    if (p_.side == Side::Sell && target < p_.limit) {
        target = p_.limit;
    }
    // Only valid prices leave (the limit itself may not be one): a buy is
    // rounded down, a sell up, so the result stays within the limit and on
    // our side of the spread.
    return p_.side == Side::Buy ? inst.round_down(target) : inst.round_up(target);
}

void ChaseOrder::on_unexpected_close() {
    if (stop_requested_) {
        // We asked for this cancel (stop() or max duration).
        finish(expired_ ? Result::Expired : Result::Canceled);
        return;
    }
    if (reprices_ >= p_.max_reprices) {
        // A new order counts as a reprice: never loop forever.
        finish(Result::Canceled);
        return;
    }
    // Not asked by us: the exchange removed the order (seen live on
    // Hyperliquid 2026-11-22: a modify refused as post-only also canceled
    // the order). Chase on with a new order for what is left.
    ++reprices_;
    state_ = State::Init;
    px_ = Price{};  // Our old price level no longer holds our quantity.
    last_request_at_ = now();
    on_book(ctx_.book);
}

void ChaseOrder::finish(Result r) {
    state_ = State::Done;
    result_ = r;
}

}  // namespace peewee
