# peewee

A C++20 library for placing and managing orders on crypto exchanges. It gives
trading algorithms one interface for market data and order entry, and handles
the exchange-specific parts: signing, wire formats, acknowledgements, fills,
reconnects.

Supported venues: Bybit (v5, linear perpetuals) and Hyperliquid (perpetuals),
plus a simulated venue for tests. Both real venues have been used with real
orders, in small size.

## What it does

Order types:

| Type | Bybit | Hyperliquid |
|---|---|---|
| Limit: GTC, IOC, post-only | yes | yes |
| Limit: FOK | yes | no (rejected before sending) |
| Market, with a worst price | yes (sent as limit IOC) | yes (limit IOC, as the exchange does itself) |
| Stop market / stop limit | yes | yes |
| Take-profit market / limit | yes | yes |
| Reduce-only | yes | yes |
| Amend price and quantity | yes | yes (modify) |

Every market-type order carries a worst acceptable price, so an empty book
cannot fill it at an absurd level. One exception: once triggered, a
Hyperliquid stop-market or take-profit-market order uses the exchange's own
10% slippage limit instead; use the limit variants for a hard bound.

Around the order types:

- Prices and quantities are integers (ticks and lots), never doubles.
- The order manager allows one request in flight per order, matches answers
  to requests, drops duplicate fills, and asks the exchange for the order
  state when an answer does not arrive in time.
- Connections are monitored with pings; a dead one is noticed within 15 s
  (Hyperliquid) or 25 s (Bybit), reconnected with backoff, and open orders are
  re-queried afterwards.
- Algorithms only react to events (book, order, timer, venue status) and never
  do I/O, so the same code runs against a real exchange or the simulator.
- Each venue runs its network I/O on its own thread; the engine and the
  algorithms run on one thread.

Included algorithm: `ChaseOrder`, a post-only order that follows the best bid
or ask up to a limit price, with a minimum interval between amends, a cap on
the number of amends and a maximum duration.

## Building

Requirements: a C++20 compiler, CMake 3.20+, Boost 1.81+ (Beast/Asio, header
only), OpenSSL 3 and simdjson. On Ubuntu 24.04:

```bash
sudo apt install build-essential cmake ninja-build libboost-dev libssl-dev libsimdjson-dev
cmake -S . -B build -G Ninja
cmake --build build
cd build && ctest
```

## Quick start

```cpp
#include <cstdio>

#include "peewee/client/exchange.hpp"

using namespace peewee;

int main() {
    // Keys from HL_ACCOUNT (main account) and HL_API_KEY (API wallet key).
    auto hl = Exchange::hyperliquid({"ETH", "BTC"});

    // Follow the best bid, paying at most 0.5% above the ask seen at start.
    auto order = hl.chase("ETH", Side::Buy, 0.01, {.max_move_pct = 0.5});
    order.wait();
    std::printf("%s: %g ETH at %g\n", order.state_text(), order.filled(), order.avg_price());

    // Other kinds of order. Each call returns at once; wait(), cancel(),
    // filled() and state() can be used from any thread.
    auto close = hl.market("ETH", Side::Sell, 0.01, {.max_move_pct = 0.3, .reduce_only = true});
    auto resting = hl.limit("BTC", Side::Buy, 0.0002, 55000.0, {.tif = Tif::PostOnly});
    close.wait();
    resting.cancel();
}
```

`Exchange::bybit({"BTCPERP"})` works the same way, with `BYBIT_API_KEY` and
`BYBIT_API_SECRET`. The exchange's tick and lot sizes are fetched when the
`Exchange` is created, so prices and quantities are plain numbers. The engine
runs on a background thread owned by the `Exchange`; destroying it cancels
the orders still working.

The rest of this page is about the lower level, for writing your own
algorithms.

## Example: a chase order on Hyperliquid, lower level

```cpp
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "peewee/algo/chase_order.hpp"
#include "peewee/engine/engine.hpp"
#include "peewee/venues/hyperliquid.hpp"

using namespace peewee;

int main() {
    const char* account = std::getenv("HL_ACCOUNT");  // main account address
    const char* key = std::getenv("HL_API_KEY");      // API wallet key, not the main key
    if (!account || !key) return 1;

    Engine engine;

    HyperliquidConfig cfg;
    cfg.testnet = true;
    cfg.account_address = account;
    cfg.api_wallet_key = key;
    auto& hl = engine.add_venue<HyperliquidVenue>(cfg);

    const auto spec = hl.fetch_instrument("ETH");
    const auto& eth = engine.add_instrument(hl, "ETH", spec.tick_size, spec.lot_size, spec.min_qty);
    engine.connect_all();

    ChaseParams p;
    p.side = Side::Buy;
    p.qty = eth.qty(0.01);
    p.max_move_pct = 0.5;                       // pay at most 0.5% above the ask seen at start
    p.min_reprice_interval = std::chrono::seconds(1);
    p.max_duration = std::chrono::minutes(2);   // then cancel
    auto& chase = engine.start_algo<ChaseOrder>(hl, eth, p);

    engine.run();  // returns when the chase is done
    std::printf("%s, filled %g\n", to_string(chase.result()), eth.to_double(chase.filled()));
    engine.shutdown();
}
```

`connect()` checks with Hyperliquid that the key belongs to an API wallet of
`account_address` and refuses to start otherwise. Keys are only ever read from
the environment; nothing is written to disk.

## Example: your own algorithm, on the simulator

```cpp
#include <cstdio>

#include "peewee/engine/engine.hpp"
#include "peewee/venues/sim.hpp"

using namespace peewee;

// Buys at market as soon as the spread is one tick wide, then stops.
class BuyOnTightSpread final : public Algo {
public:
    BuyOnTightSpread(AlgoContext ctx, Qty qty) : Algo(ctx), qty_(qty) {}

    void on_book(const OrderBook& book) override {
        if (cid_ != kNoOrder || !book.valid()) return;
        const auto bid = book.best_bid();
        const auto ask = book.best_ask();
        if (!bid || !ask || *ask - *bid > 1) return;

        NewOrder o;
        o.inst = ctx_.instrument.id;
        o.side = Side::Buy;
        o.type = OrderType::Market;
        o.px = *ask + 2;  // worst price accepted
        o.qty = qty_;
        cid_ = ctx_.oms.send_new(id(), ctx_.venue, o);
    }

    void on_order(const Order& order, const OrderEvent&) override { done_ = is_terminal(order.state); }
    bool done() const override { return done_; }

private:
    Qty qty_;
    ClientOrderId cid_ = kNoOrder;
    bool done_ = false;
};

int main() {
    ManualClock clock;
    Engine engine(clock);
    auto& sim = engine.add_venue<SimVenue>(clock);
    const auto& btc = engine.add_instrument(sim, "BTC", 0.1, 0.001, 0.001);

    sim.set_book(btc.id, {{btc.price(100.0), btc.qty(1)}}, {{btc.price(100.5), btc.qty(1)}});
    auto& algo = engine.start_algo<BuyOnTightSpread>(sim, btc, btc.qty(0.01));
    engine.poll();

    sim.set_book(btc.id, {{btc.price(100.4), btc.qty(1)}}, {{btc.price(100.5), btc.qty(1)}});
    while (engine.poll() > 0) {}

    std::printf("done: %s\n", algo.done() ? "yes" : "no");
}
```

The simulator can also inject latency, rate limits, rejects, lost responses,
dropped streams and duplicate fills; see `include/peewee/venues/sim.hpp` and
the tests.

## Command-line tools

Built in `build/` together with the library:

```text
chase_live <bybit|hl> <test|main> SYMBOL buy|sell QTY [LIMIT] [options]
    --max-move P%      without LIMIT: limit = best ask + P% (buy) / best bid - P% (sell)
    --reprice-ms N     minimum time between amends
    --offset N         ticks in front of the best price
    --max-reprices N   amends allowed in total
    --max-seconds N    cancel and stop after N seconds
hl_book, bybit_book    print the top of the book (no keys needed)
chase_sim              a chase on the simulator, step by step
```

Keys come from the environment: `BYBIT_API_KEY` and `BYBIT_API_SECRET`, or
`HL_ACCOUNT` and `HL_API_KEY`. `PEEWEE_DEBUG=1` prints every message sent and
received (except order book updates). Those logs contain your account
address.

## Layout

```text
include/peewee/client   Exchange and OrderHandle: the simple interface
include/peewee/core     prices, quantities, order types, commands, events, clock
include/peewee/oms      order state and the order manager
include/peewee/algo     algorithm base class and ChaseOrder
include/peewee/engine   event loop connecting venues, OMS and algorithms
include/peewee/venues   Bybit, Hyperliquid and simulator adapters
src/venues/*/           *_codec.cpp: pure translation, tested on real messages
                        *_venue.cpp: connections and threads
src/transport           WebSocket and HTTPS clients (Boost.Beast)
tests/data              real exchange messages, with account identifiers replaced
examples                the command-line tools above
```

Modules depend on each other in one direction only
(`client -> engine -> algo -> book, oms -> core`; `venues -> core, transport`). A build
step (`cmake/check_layers.cmake`) fails the build if a file includes a module
it should not.

## Limitations

- No positions or balances yet: the library tracks orders, not what they add
  up to.
- One instrument per algorithm.
- Order rate limits are not enforced for Hyperliquid (they depend on the
  account's traded volume).
- Bybit has been tested with linear contracts only.
- Hyperliquid prices are limited to 5 significant digits. The tick used for an
  instrument is computed from its price at startup, and is only valid while the
  price stays in the same power of ten.
- Not supported: TWAP, trailing stops, and take-profit / stop-loss attached
  to an entry order.

## Disclaimer

This is personal project code. Test with small amounts and on testnet first.
It comes with no warranty: you are responsible for every order it sends.

## License

MIT, see `LICENSE`. The ECDSA signing code is adapted from
[hyperliquid-cpp](https://github.com/charlitocrc/hyperliquid-cpp), see
`THIRD_PARTY_NOTICES.md`.
