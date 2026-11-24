// Simulated scenario: the price rises in steps, then someone sells to us.
// Prints chase activity at every step. While chase_order.cpp is empty,
// you will see "no requests": this is your manual test harness.

#include <cstdio>

#include "peewee/algo/chase_order.hpp"
#include "peewee/engine/engine.hpp"
#include "peewee/venues/sim.hpp"

using namespace peewee;
using namespace std::chrono_literals;

namespace {

void report(const char* step, const ChaseOrder& c, const SimVenue& sim, const Instrument& inst) {
    std::printf("\n== %s\n", step);
    std::printf("   chase: state=%s price=%.1f filled=%.3f/%.3f amend=%d result=%s\n",
                to_string(c.state()), inst.to_double(c.current_price()), inst.to_double(c.filled()),
                inst.to_double(c.params().qty), c.reprice_count(), to_string(c.result()));
    if (sim.requests().empty()) {
        std::printf("   (no requests sent to the exchange)\n");
        return;
    }
    std::printf("   requests received by the exchange so far:\n");
    for (const auto& r : sim.requests()) {
        std::printf("     t=%5lldms  %-6s cid=%llu px=%.1f\n",
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(r.ts).count()),
                    to_string(r.kind), static_cast<unsigned long long>(r.cid), inst.to_double(r.px));
    }
}

}  // namespace

int main() {
    ManualClock clock;
    Engine engine(clock);

    SimConfig cfg;
    cfg.latency = 2ms;
    auto& sim = engine.add_venue<SimVenue>(clock, cfg);
    const auto& btc = engine.add_instrument(sim, "BTCUSDT", 0.1, 0.001, 0.001);

    auto run_for = [&](Duration d) {
        const Timestamp end = clock.now() + d;
        while (clock.now() < end) {
            clock.advance(1ms);
            while (engine.poll() > 0) {}
        }
    };

    sim.set_book(btc.id, {{btc.price(100.0), btc.qty(1)}}, {{btc.price(100.5), btc.qty(1)}});

    ChaseParams p;
    p.side = Side::Buy;
    p.qty = btc.qty(0.01);
    p.limit = btc.price(100.3);
    auto& chase = engine.start_algo<ChaseOrder>(sim, btc, p);

    run_for(20ms);
    report("startup (bid 100.0 / ask 100.5)", chase, sim, btc);

    sim.update_book(btc.id, {{btc.price(100.1), btc.qty(2)}}, {});
    run_for(100ms);
    report("someone bids at 100.1", chase, sim, btc);

    sim.update_book(btc.id, {{btc.price(100.2), btc.qty(1)}}, {});
    run_for(100ms);
    report("someone bids at 100.2", chase, sim, btc);

    sim.update_book(btc.id, {{btc.price(100.4), btc.qty(1)}}, {});
    run_for(100ms);
    report("someone bids at 100.4 (above our 100.3 limit)", chase, sim, btc);

    sim.trade(btc.id, Side::Sell, btc.price(100.3), btc.qty(0.01));
    run_for(20ms);
    report("a seller hits our order", chase, sim, btc);

    return 0;
}
