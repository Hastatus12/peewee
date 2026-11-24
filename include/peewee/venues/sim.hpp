#pragma once
// Simulated exchange.
//
// Behaves like a real venue but is controlled by the test or example:
// you decide how the book moves, when orders are filled,
// network latency, and when requests are rejected.
//
// Simplifications compared with a real exchange:
//  - always publishes full book snapshots (real exchanges send deltas);
//  - a non-post-only order crossing the spread is fully filled
//    at the best opposite price, ignoring depth (so FOK behaves like IOC);
//  - trigger orders use the last trade() price, or the book's mid price
//    when the book changes, as their "mark price";
//  - requests change exchange state immediately; only RESPONSES
//    (and market data) arrive after `latency`.

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "peewee/book/order_book.hpp"
#include "peewee/core/clock.hpp"
#include "peewee/core/rate_limiter.hpp"
#include "peewee/venue/venue.hpp"

namespace peewee {

struct SimConfig {
    std::string name = "sim";
    VenueCapabilities caps{};
    Duration latency = Duration::zero();  // Delay before events and responses arrive.
    double max_requests_per_sec = 0.0;    // 0 = no rate limit.
    double request_burst = 10.0;
    // Like Hyperliquid: a refused amend also CANCELS the order (a modify is
    // "cancel + place", and the place failed). false = like Bybit.
    bool amend_reject_cancels = false;
};

class SimVenue final : public IVenue {
public:
    // Log of every received request: useful in tests ("how many amends were sent?").
    struct Request {
        RequestKind kind;
        ClientOrderId cid;
        Price px;
        Timestamp ts;
    };

    struct SimOrder {
        ClientOrderId cid = kNoOrder;
        InstrumentId inst = 0;
        Side side = Side::Buy;
        OrderType type = OrderType::Limit;
        Tif tif = Tif::GTC;
        Price px;
        Qty qty;
        Qty filled;
        Price trigger_px;
        bool open = false;
        bool waiting_trigger = false;  // Trigger order not triggered yet: not in the book.
    };

    SimVenue(VenueId id, const IClock& clock, SimConfig cfg = {});

    // --- IVenue ---
    VenueId id() const override { return id_; }
    std::string_view name() const override { return cfg_.name; }
    const VenueCapabilities& caps() const override { return cfg_.caps; }
    void set_sink(IEventSink* sink) override { sink_ = sink; }
    void add_instrument(const Instrument& inst) override;
    void connect() override {}
    void disconnect() override {}
    void subscribe_book(InstrumentId inst, int depth) override;
    void subscribe_private() override {}
    void send_new(const NewOrder& o) override;
    void send_amend(const AmendOrder& o) override;
    void send_cancel(const CancelOrder& o) override;
    void query_order(const QueryOrder& q) override;
    void resync() override;
    bool can_send(RequestKind kind) const override;
    void poll() override;  // Deliver events whose delay has elapsed.

    // --- Controls for tests and examples ---

    // Replace other participants' book (your orders are added on top).
    void set_book(InstrumentId inst, std::vector<Level> bids, std::vector<Level> asks);
    // Update selected levels (qty 0 = remove).
    void update_book(InstrumentId inst, std::vector<Level> bids, std::vector<Level> asks);
    // Another participant trades at market: aggressor = Sell hits our BUY orders priced >= px.
    void trade(InstrumentId inst, Side aggressor, Price px, Qty qty);
    // Reject the next request of this type with this reason.
    void reject_next(RequestKind kind, RejectReason reason);
    // Simulate the public stream going down or coming back.
    void set_public_stream(bool up);

    // --- Failure injection ---

    // The next request of this type is APPLIED by the exchange, but its
    // response is lost: the OMS sees no answer (unknown outcome).
    void drop_next_response(RequestKind kind);
    // Simulate the private stream going down or coming back. While down,
    // fills and closes are lost (request responses still arrive).
    void set_private_stream(bool up);
    // Deliver the last fill again, as some exchanges do after a reconnect.
    void replay_last_fill();
    // Number of QueryOrder requests received.
    size_t query_count() const { return queries_; }

    const std::vector<Request>& requests() const { return requests_; }
    size_t count_requests(RequestKind kind) const;
    const SimOrder* order(ClientOrderId cid) const;
    // Number of events awaiting delivery due to latency.
    size_t pending_events() const { return outbox_.size(); }

private:
    struct Pending {
        Timestamp at;
        Event ev;
    };

    void emit(Event e);
    // Response to a request: dropped if drop_next_response() was armed.
    void respond(RequestKind kind, OrderEvent e);
    // Unsolicited private-stream event (fill, close): dropped while the private stream is down.
    void emit_private(OrderEvent e);
    void emit_fill(const SimOrder& o, Price px, Qty q, bool maker);
    void publish_book(InstrumentId inst);
    bool crosses(const SimOrder& o, Price px) const;
    void fill_as_taker(SimOrder& o);
    // An order that just entered the book: fill it if it crosses, else cancel
    // it if it may not rest (IOC, FOK, market types).
    void enter_book(SimOrder& o);
    // Trigger the waiting orders of `inst` whose condition `mark` meets.
    void check_triggers(InstrumentId inst, Price mark);
    std::optional<Price> mid(InstrumentId inst) const;
    // After a refused amend, with SimConfig::amend_reject_cancels.
    void close_after_amend_reject(ClientOrderId cid);
    bool check_reject(RequestKind kind, ClientOrderId cid, RequestId req);
    Timestamp now() const { return clock_.now(); }
    Timestamp ts() const { return clock_.wall(); }  // Timestamp stamped on events.

    VenueId id_;
    const IClock& clock_;
    SimConfig cfg_;
    IEventSink* sink_ = nullptr;
    std::optional<TokenBucket> limiter_;
    std::array<std::optional<RejectReason>, 3> reject_next_{};
    std::array<bool, 3> drop_next_response_{};
    bool public_up_ = true;
    bool private_up_ = true;
    uint64_t next_exec_ = 1;
    size_t queries_ = 0;
    std::optional<Fill> last_fill_;

    std::unordered_map<InstrumentId, OrderBook> books_;     // Other participants' books.
    std::unordered_map<InstrumentId, bool> subscribed_;
    std::unordered_map<InstrumentId, uint64_t> seq_;
    std::unordered_map<ClientOrderId, SimOrder> orders_;
    std::vector<Request> requests_;
    std::deque<Pending> outbox_;
};

}  // namespace peewee
