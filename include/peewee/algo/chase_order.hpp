#pragma once
// ============================================================================
//  CHASE ORDER: specification. Implementing chase_order.cpp is your exercise.
// ============================================================================
//
// Goal: buy (or sell) `qty` while staying at the front of the queue
// on your side of the book, as maker, without exceeding `limit`.
//
// Expected behavior (also verified by the tests in
// tests/test_chase_order.cpp):
//
//  1. At startup, place an order at the target price as soon as the book is valid.
//  2. Target price (BUY example):
//        target = best_bid + offset_ticks
//     where best_bid is the best price of OTHER participants: your
//     order appears in the book like everyone else's and must be excluded,
//     otherwise offset_ticks > 0 would make you chase yourself.
//     Then constrain the result so that:
//        - if post_only: target < best_ask  (must not cross the spread)
//        - target <= limit                 (never pay more than the limit)
//     SELL is symmetric (best_ask - offset_ticks, target > best_bid,
//     target >= limit).
//  3. When the book changes and the target differs from the current price,
//     move the order (amend).
//  4. Do not move the order more often than min_reprice_interval or more than
//     max_reprices times in total.
//  5. Never have more than one request awaiting a response at a time.
//  6. Handle partial fills: remaining quantity is qty - filled.
//  7. Finish (done() == true) when:
//        - all quantity has been filled          -> Result::Filled
//        - stop() was called                     -> Result::Canceled
//        - the order was permanently rejected    -> Result::Rejected
//        - max_duration has elapsed              -> Result::Expired
//  8. Take no action if the book is invalid (e.g. disconnected).
//
// Decided:
//  - Alone ahead of the book (others behind): STAY at our price, never
//    retreat; only move forward when overtaken (test never_retreats_when_alone_ahead).
//
// Open questions for you to decide (not covered by tests):
//  - What if the exchange does not support amend (caps().native_amend == false)?
//  - What happens if a fill arrives while an amend is in flight?
// ============================================================================

#include "peewee/algo/algo.hpp"

namespace peewee {

using namespace std::chrono_literals;

struct ChaseParams {
    Side side = Side::Buy;
    Qty qty;                                  // Total quantity to execute.
    Price limit;                              // BUY: maximum price; SELL: minimum price.
    double max_move_pct = 0.0;                // Used when limit is 0: the limit is set from the
                                              // first valid book, BUY: best ask + this %,
                                              // SELL: best bid - this % (e.g. 1.0 = 1%).
    int offset_ticks = 0;                     // 0 = match the best price,
                                              // +1 = one tick more aggressive (inside the spread).
    bool post_only = true;                    // Maker only.
    Duration min_reprice_interval = 50ms;     // Minimum time between amendments.
    int max_reprices = 10'000;                // Maximum number of amendments.
    Duration max_duration = Duration::zero(); // 0 = no time limit.
};

class ChaseOrder final : public Algo {
public:
    // Suggested states. Change them if needed.
    enum class State : uint8_t {
        Init,           // Created, no order sent.
        PendingNew,     // Order sent, awaiting confirmation.
        Working,        // Order in the book, no pending request.
        PendingAmend,   // Amend sent, awaiting response.
        PendingCancel,  // Cancel sent, awaiting response.
        Done,           // Finished (see result()).
    };

    enum class Result : uint8_t { None, Filled, Canceled, Rejected, Expired };

    ChaseOrder(AlgoContext ctx, ChaseParams params);

    // --- Events (implement in chase_order.cpp) ---
    void on_start() override;
    void on_book(const OrderBook& book) override;
    void on_order(const Order& order, const OrderEvent& event) override;
    void on_timer() override;
    void on_venue_status(const VenueStatus& status) override;
    void stop() override;
    bool done() const override;

    // --- Inspection (used by tests and logs) ---
    State state() const { return state_; }
    Result result() const { return result_; }
    const ChaseParams& params() const { return p_; }
    ClientOrderId order_id() const { return cid_; }
    Price current_price() const { return px_; }
    Qty filled() const { return filled_; }
    Qty remaining() const { return p_.qty - filled_; }
    // The limit in use (computed from max_move_pct if it was not given).
    Price limit() const { return p_.limit; }
    int reprice_count() const { return reprices_; }

private:
    // Compute the price where the order SHOULD be, given the current book.
    Price compute_target(const OrderBook& book) const;

    // Transition the algorithm to Done with the specified result.
    void finish(Result r);

    // The order closed without filling everything. If we did not ask for it
    // (e.g. Hyperliquid cancels the order when a modify is rejected), place a
    // new one for the rest; otherwise finish.
    void on_unexpected_close();

    ChaseParams p_;
    State state_ = State::Init;
    Result result_ = Result::None;

    ClientOrderId cid_ = kNoOrder;  // Currently managed order.
    Price px_;                      // Exchange-confirmed price.
    Qty filled_;                    // Quantity filled so far.
    int reprices_ = 0;              // Amendments sent.
    Timestamp started_at_{};
    Timestamp last_request_at_{};

    // Add any additional state you need here.
    bool expired_ = false;
    bool opaque_book_ = false;
    bool stop_requested_ = false;   // stop() was called: keep cancelling, never amend.
};

constexpr const char* to_string(ChaseOrder::State s) {
    switch (s) {
        case ChaseOrder::State::Init:          return "Init";
        case ChaseOrder::State::PendingNew:    return "PendingNew";
        case ChaseOrder::State::Working:       return "Working";
        case ChaseOrder::State::PendingAmend:  return "PendingAmend";
        case ChaseOrder::State::PendingCancel: return "PendingCancel";
        case ChaseOrder::State::Done:          return "Done";
    }
    return "?";
}

constexpr const char* to_string(ChaseOrder::Result r) {
    switch (r) {
        case ChaseOrder::Result::None:     return "None";
        case ChaseOrder::Result::Filled:   return "Filled";
        case ChaseOrder::Result::Canceled: return "Canceled";
        case ChaseOrder::Result::Rejected: return "Rejected";
        case ChaseOrder::Result::Expired:  return "Expired";
    }
    return "?";
}

}  // namespace peewee
