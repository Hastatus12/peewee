#pragma once
// Bybit adapter (API v5): translates between Bybit and the library.

#include <memory>
#include <string>
#include <unordered_map>

#include "peewee/venue/venue.hpp"

namespace peewee {

// Without api_key only public data works (order books): requests are
// rejected with VenueUnavailable.
struct BybitConfig {
    std::string api_key;
    std::string api_secret;
    bool testnet = true;
    std::string category = "linear";   // linear (USDT perpetual), spot, inverse, option.
};

// Trading rules of one instrument, as Bybit publishes them.
struct BybitInstrumentSpec {
    double tick_size = 0.0;  // Minimum price increment.
    double lot_size = 0.0;   // Minimum quantity increment.
    double min_qty = 0.0;    // Minimum order quantity.
};

class BybitVenue final : public IVenue {
public:
    BybitVenue(VenueId id, BybitConfig cfg);
    ~BybitVenue() override;

    // Ask Bybit for a symbol's tick/lot sizes (public REST, no API key).
    // Blocking: call at startup, before connect(). Throws if the symbol
    // does not exist in cfg.category or Bybit cannot be reached.
    BybitInstrumentSpec fetch_instrument(std::string_view symbol) const;

    VenueId id() const override { return id_; }
    std::string_view name() const override { return "bybit"; }
    const VenueCapabilities& caps() const override { return caps_; }
    void set_sink(IEventSink* sink) override { sink_ = sink; }
    void add_instrument(const Instrument& inst) override;
    // Also checks every instrument's tick/lot sizes against Bybit and
    // throws on a mismatch: wrong sizes would round prices to wrong values.
    void connect() override;
    void disconnect() override;
    void subscribe_book(InstrumentId inst, int depth) override;
    void subscribe_private() override;
    void send_new(const NewOrder& o) override;
    void send_amend(const AmendOrder& o) override;
    void send_cancel(const CancelOrder& o) override;
    void query_order(const QueryOrder& q) override;
    void resync() override;
    bool can_send(RequestKind kind) const override;

private:
    VenueId id_;
    BybitConfig cfg_;
    VenueCapabilities caps_;
    IEventSink* sink_ = nullptr;

    // Filled by add_instrument() before connect(), read-only afterwards
    // (unordered_map nodes never move, so pointers to them stay valid).
    std::unordered_map<InstrumentId, Instrument> instruments_;
    std::unordered_map<std::string, InstrumentId> by_symbol_;

    // Network state (Boost.Asio), kept out of this public header.
    // Bybit uses three WebSocket connections (public, private, trade) plus REST.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace peewee
