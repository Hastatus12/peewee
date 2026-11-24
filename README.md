# peewee

C++20 library for order execution across multiple exchanges (Bybit, Hyperliquid, ...).

## Build and run

```bash
cmake -S . -B build -G Ninja
cmake --build build
cd build && ctest --output-on-failure   # all tests
./tests/test_chase_order                # chase tests (skipped until enabled)
./chase_sim                             # simulated chase scenario
```

No external dependencies are required until you implement the networking layer
(`-DPEEWEE_WITH_NETWORK=ON`, see `CMakeLists.txt`).

## Structure

```text
include/peewee/             PUBLIC HEADERS
  core/                    shared types: Price, Qty, events, commands, clock, SPSC queue, rate limiter
  book/                    local order book
  venue/                   IVenue: the interface every exchange must implement
  oms/                     order state and OrderManager
  algo/                    Algo base class + ChaseOrder (specification)
  engine/                  Engine: dispatches events and connects the components
  venues/                  public adapter headers (sim, bybit, hyperliquid)
src/
  book/ oms/ engine/        implementations
  algo/chase_order.cpp     <- YOUR EXERCISE
  transport/               generic WebSocket and HTTP clients (private)
  venues/sim/              simulated exchange                            [working]
  venues/bybit/            Bybit adapter: venue, codec, auth
  venues/hyperliquid/       Hyperliquid adapter: venue, codec, signer     [stub]
tests/                     tests (sim_harness.hpp = simulator test harness)
examples/                  chase_sim (simulated), chase_live (Bybit testnet),
                           bybit_book (Bybit order book, no API key needed)
```

Dependencies flow in one direction:

```text
engine -> algo -> { book, oms } -> core + venue (interface)
venues/* -> core (+ transport, private)
```

The compiler alone does not enforce this (every target sees `include/`): the
`check_layers` build step does, failing the build when a module includes one
it may not depend on. Rules: `cmake/check_layers.cmake`.

## Runtime rules

- **One request per order.** The OMS refuses `send_amend`/`send_cancel` while a
  request is pending (they return `false`). An order stays `Live`, and can be
  filled, while its amend or cancel is in flight (`Order::pending`).
- **Responses are matched by `RequestId`**; stale ones are dropped.
- **Duplicate fills** (same `exec_id`) are dropped.
- **Unknown outcomes.** No response within `request_timeout`: the OMS queries
  the order and trusts the `OrderStatus` answer. It does the same for every
  open order when the private stream reconnects.
- **Threading.** Each venue delivers events from one single network thread
  (one shared `io_context` for all its connections).
- **Queue overflow** is treated as a disconnection: books are invalidated,
  algorithms are told, and the venue resyncs.
- **Clock.** `IClock::now()` is monotonic (intervals, deadlines); `wall()` is calendar time.
- **Signals.** From a signal handler call only `Engine::request_stop_algos()`.

## Status

| Component | Status |
|---|---|
| core, book, oms, engine | Implemented and tested |
| venues/sim | Implemented and tested |
| algo/chase_order | Implemented and tested |
| transport | Implemented (WebSocket + HTTPS, Boost.Beast) |
| bybit | Implemented; order book verified live, orders not yet tried on testnet |
| hyperliquid | Interfaces + TODOs |

Before connecting real money, still to do: try orders on the Bybit testnet and
save real messages in `tests/data/`, fall back to `/v5/order/history` in
`query_order`, prune
closed orders and old exec IDs in the OMS, and a startup reconciliation of
orders left open by a previous run.

## Exercise: ChaseOrder

1. Read the specification in `include/peewee/algo/chase_order.hpp`.
2. Open `src/algo/chase_order.cpp`: each method contains guiding questions.
3. In `tests/test_chase_order.cpp`, remove `SKIP_UNTIL_IMPLEMENTED();` from one test
   at a time, in the order they appear, and make it pass.
4. Use `./chase_sim` to observe your chase algorithm step by step.

## Next steps after implementing chase

1. Run `chase_live` on the Bybit testnet with a testnet API key.
2. Implement `venues/hyperliquid`, verifying signatures against the official Python SDK.
