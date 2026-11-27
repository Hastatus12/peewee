#pragma once
// Test harness: engine + simulated exchange + manual clock.
// Initial BTCUSDT book (tick 0.1, lot 0.001):
//     bid 100.0 x 1      ask 100.5 x 1

#include "check.hpp"
#include "peewee/engine/engine.hpp"
#include "peewee/venues/sim.hpp"

namespace peewee::test {

using namespace std::chrono_literals;

struct SimHarness {
    // price_sig_figs: as Hyperliquid's limit (0 = none). With 3, prices from
    // 100 up must be whole numbers, below 100 one decimal is allowed.
    explicit SimHarness(SimConfig cfg = {}, Engine::Options opt = {}, int price_sig_figs = 0)
        : engine(clock, opt),
          sim(engine.add_venue<SimVenue>(clock, cfg)),
          btc(engine.add_instrument(sim, "BTCUSDT", 0.1, 0.001, 0.001, price_sig_figs)) {
        clock.set(1s);
        sim.set_book(btc.id, {{px(100.0), q(1)}}, {{px(100.5), q(1)}});
    }

    Price px(double v) const { return btc.price(v); }
    Qty q(double v) const { return btc.qty(v); }

    // Process all ready events, including those generated while processing.
    void pump() {
        for (int i = 0; i < 1000; ++i) {
            if (engine.poll() == 0) return;
        }
        std::printf("    pump(): too many events, possible infinite loop\n");
        throw Failure{};
    }

    // Advance time and process events that have become ready.
    void advance(Duration d) {
        clock.advance(d);
        pump();
    }

    ManualClock clock;
    Engine engine;
    SimVenue& sim;
    const Instrument& btc;
};

}  // namespace peewee::test
