// Live test of the new order types on Bybit, like the two Hyperliquid tests
// (hl_trigger_test + hl_stop_fill_test) in one run:
//
//  A. a STOP-MARKET BUY triggering 5% above the market (cannot trigger):
//     accepted as "Untriggered", moved 1 tick, cancelled;
//  B. a MARKET BUY whose worst price is 5% below the bid: nothing to buy,
//     Bybit must cancel it at once (price protection of market orders);
//  C. a STOP-MARKET BUY triggering just above the market (+0.02%): waits up
//     to 5 minutes; if it fills, the position is closed at once with a
//     reduce-only MARKET SELL; if not, the stop is cancelled.
//
// Each order is worth about 6 USD (the symbol's minimum if larger). Only C can
// trade: it costs a few cents of fees.
//
//   BYBIT_API_KEY=... BYBIT_API_SECRET=... PEEWEE_DEBUG=1 ./bybit_order_types_test main BTCPERP
//
// If it dies halfway: on bybit.com cancel the open orders (also "Conditional")
// and close the small position, if any.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "peewee/engine/engine.hpp"
#include "peewee/venues/bybit.hpp"

using namespace peewee;

namespace {

using Clock = std::chrono::steady_clock;

// Runs the engine until `done(order)` is true or the time is up.
template <typename Done>
bool wait_for(Engine& engine, ClientOrderId cid, std::chrono::seconds max, const Instrument& inst, Done done) {
    const auto end = Clock::now() + max;
    auto next_print = Clock::now() + std::chrono::seconds(15);
    while (Clock::now() < end) {
        engine.poll();
        const Order* o = engine.oms().find(cid);
        if (o && done(*o)) return true;
        if (Clock::now() >= next_print) {
            const auto bid = engine.book(inst.id).best_bid();
            std::printf("... waiting: order %s, best bid %g\n", o ? to_string(o->state) : "?",
                        bid ? inst.to_double(*bid) : 0.0);
            next_print = Clock::now() + std::chrono::seconds(15);
        }
    }
    return false;
}

bool closed(const Order& o) { return is_terminal(o.state); }
bool acked(const Order& o) { return o.state != OrderState::PendingNew; }

const char* state_of(Engine& engine, ClientOrderId cid) {
    const Order* o = engine.oms().find(cid);
    return o ? to_string(o->state) : "?";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || (std::strcmp(argv[1], "test") != 0 && std::strcmp(argv[1], "main") != 0)) {
        std::fprintf(stderr, "usage: BYBIT_API_KEY=... BYBIT_API_SECRET=... PEEWEE_DEBUG=1 %s <test|main> <SYMBOL>\n",
                     argv[0]);
        return 2;
    }
    const bool mainnet = std::strcmp(argv[1], "main") == 0;
    const std::string symbol = argv[2];
    const char* key = std::getenv("BYBIT_API_KEY");
    const char* secret = std::getenv("BYBIT_API_SECRET");
    if (!key || !secret) {
        std::fprintf(stderr, "error: set BYBIT_API_KEY and BYBIT_API_SECRET\n");
        return 2;
    }

    Engine engine;
    BybitConfig cfg;
    cfg.api_key = key;
    cfg.api_secret = secret;
    cfg.testnet = !mainnet;
    auto& bybit = engine.add_venue<BybitVenue>(cfg);

    const Instrument* inst_ptr = nullptr;
    double min_qty = 0.0;
    try {
        const auto spec = bybit.fetch_instrument(symbol);
        min_qty = spec.min_qty;
        inst_ptr = &engine.add_instrument(bybit, symbol, spec.tick_size, spec.lot_size, spec.min_qty);
        std::printf("%s %s: tick %g, lot %g, min qty %g\n", mainnet ? "MAINNET (real money)" : "testnet",
                    symbol.c_str(), spec.tick_size, spec.lot_size, spec.min_qty);
        engine.connect_all();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    const Instrument& inst = *inst_ptr;
    bybit.subscribe_book(inst.id, 50);

    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while ((!engine.book(inst.id).best_bid() || !engine.book(inst.id).best_ask()) && Clock::now() < deadline) {
        engine.poll();
    }
    if (!engine.book(inst.id).best_bid() || !engine.book(inst.id).best_ask()) {
        std::fprintf(stderr, "error: no book after 15 s\n");
        engine.shutdown();
        return 1;
    }
    const double bid = inst.to_double(*engine.book(inst.id).best_bid());
    const double ask = inst.to_double(*engine.book(inst.id).best_ask());

    // ~6 USD, at least the minimum quantity, in whole lots.
    const double wanted = std::max(min_qty, 6.0 / ask);
    const Qty qty{static_cast<int64_t>(std::ceil(wanted / inst.lot_size - 1e-9))};
    std::printf("best bid %g, ask %g; every order: %g %s (~%.2f USD)\n", bid, ask, inst.to_double(qty),
                symbol.c_str(), inst.to_double(qty) * ask);

    // --- A. Stop that cannot trigger: ack, amend, cancel ---
    NewOrder far;
    far.inst = inst.id;
    far.side = Side::Buy;
    far.type = OrderType::StopMarket;
    far.trigger_px = inst.price(ask * 1.05);
    far.px = inst.price(ask * 1.055);
    far.qty = qty;
    std::printf("\n=== A. STOP-MARKET BUY: trigger %g, worst %g (cannot trigger) ===\n", inst.to_double(far.trigger_px),
                inst.to_double(far.px));
    const ClientOrderId far_cid = engine.oms().send_new(0, bybit, far);
    wait_for(engine, far_cid, std::chrono::seconds(10), inst, acked);
    std::printf("=== A. state: %s ===\n", state_of(engine, far_cid));

    if (engine.oms().send_amend(far_cid, far.px + 1)) {
        std::printf("\n=== A. amending the worst price %g -> %g ===\n", inst.to_double(far.px),
                    inst.to_double(far.px + 1));
        wait_for(engine, far_cid, std::chrono::seconds(10), inst, [](const Order& o) { return !o.has_pending(); });
    }
    std::printf("\n=== A. cancelling ===\n");
    engine.oms().send_cancel(far_cid);
    wait_for(engine, far_cid, std::chrono::seconds(10), inst, closed);
    std::printf("=== A. state: %s (expected Canceled) ===\n", state_of(engine, far_cid));

    // --- B. Market that must not fill ---
    NewOrder market;
    market.inst = inst.id;
    market.side = Side::Buy;
    market.type = OrderType::Market;
    market.px = inst.price(bid * 0.95);
    market.qty = qty;
    std::printf("\n=== B. MARKET BUY, worst %g (below the market: must NOT fill) ===\n", inst.to_double(market.px));
    const ClientOrderId market_cid = engine.oms().send_new(0, bybit, market);
    wait_for(engine, market_cid, std::chrono::seconds(10), inst, closed);
    std::printf("=== B. state: %s (expected Canceled) ===\n", state_of(engine, market_cid));

    // --- C. Stop that triggers ---
    const double ask_now = inst.to_double(*engine.book(inst.id).best_ask());
    NewOrder stop;
    stop.inst = inst.id;
    stop.side = Side::Buy;
    stop.type = OrderType::StopMarket;
    stop.trigger_px = inst.price(ask_now * 1.0002) + 1;  // At least 1 tick above the ask.
    stop.px = inst.price(ask_now * 1.005);
    stop.qty = qty;
    std::printf("\n=== C. STOP-MARKET BUY: trigger %g, worst %g (best ask %g): waiting up to 5 minutes ===\n",
                inst.to_double(stop.trigger_px), inst.to_double(stop.px), ask_now);
    const ClientOrderId stop_cid = engine.oms().send_new(0, bybit, stop);
    wait_for(engine, stop_cid, std::chrono::seconds(300), inst, closed);

    const Order* s = engine.oms().find(stop_cid);
    if (s && s->state == OrderState::Filled) {
        NewOrder close;
        close.inst = inst.id;
        close.side = Side::Sell;
        close.type = OrderType::Market;
        close.px = inst.price(inst.to_double(*engine.book(inst.id).best_bid()) * 0.99);
        close.qty = stop.qty;
        close.reduce_only = true;
        std::printf("\n=== C. stop FILLED: closing with a reduce-only MARKET SELL, worst %g ===\n",
                    inst.to_double(close.px));
        const ClientOrderId close_cid = engine.oms().send_new(0, bybit, close);
        wait_for(engine, close_cid, std::chrono::seconds(15), inst, closed);
        std::printf("=== C. close order: %s (expected Filled) ===\n", state_of(engine, close_cid));
    } else {
        std::printf("\n=== C. the stop did not fill (state %s): cancelling it ===\n", state_of(engine, stop_cid));
        engine.oms().send_cancel(stop_cid);
        wait_for(engine, stop_cid, std::chrono::seconds(10), inst, closed);
    }

    const auto end = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < end) engine.poll();
    std::printf("\n=== done: check on bybit.com that no order (also Conditional) and no position are left ===\n");
    engine.shutdown();
    return 0;
}
