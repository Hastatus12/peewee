// Live test of Lighter order entry: places ONE post-only BTC buy 20% below
// the best bid (so it cannot fill), moves it one tick lower, cancels it, and
// prints every raw message Lighter sends back. Those messages are the
// examples to save in tests/data/lighter/ (the sendtx answer and the
// account_all_orders updates are not recorded yet).
//
//   LIGHTER_ACCOUNT_INDEX=<n> LIGHTER_API_KEY_INDEX=<n> LIGHTER_API_KEY=<hex> PEEWEE_DEBUG=1 ./lighter_order_test <test|main>
//
// The order is worth about 12 USD (Lighter's minimum is 10). If the program
// dies before cancelling, cancel it on app.lighter.xyz.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "peewee/engine/engine.hpp"
#include "peewee/venues/lighter.hpp"

using namespace peewee;

namespace {

void run_for(Engine& engine, std::chrono::seconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) engine.poll();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || (std::strcmp(argv[1], "test") != 0 && std::strcmp(argv[1], "main") != 0)) {
        std::fprintf(stderr,
                     "usage: LIGHTER_ACCOUNT_INDEX=... LIGHTER_API_KEY_INDEX=... LIGHTER_API_KEY=... "
                     "PEEWEE_DEBUG=1 %s <test|main>\n",
                     argv[0]);
        return 2;
    }
    const bool mainnet = std::strcmp(argv[1], "main") == 0;
    const char* account = std::getenv("LIGHTER_ACCOUNT_INDEX");
    const char* key_index = std::getenv("LIGHTER_API_KEY_INDEX");
    const char* key = std::getenv("LIGHTER_API_KEY");
    if (!account || !key_index || !key) {
        std::fprintf(stderr, "error: set LIGHTER_ACCOUNT_INDEX, LIGHTER_API_KEY_INDEX and LIGHTER_API_KEY\n");
        return 2;
    }

    Engine engine;
    LighterConfig cfg;
    cfg.testnet = !mainnet;
    cfg.account_index = std::atoll(account);
    cfg.api_key_index = std::atoi(key_index);
    cfg.api_private_key = key;
    auto& lighter = engine.add_venue<LighterVenue>(cfg);

    const Instrument* btc_ptr = nullptr;
    try {
        const auto spec = lighter.fetch_instrument("BTC");
        btc_ptr = &engine.add_instrument(lighter, "BTC", spec.tick_size, spec.lot_size, spec.min_qty);
        engine.connect_all();  // Also checks the API key against Lighter.
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    const Instrument& btc = *btc_ptr;
    lighter.subscribe_book(btc.id, 0);
    lighter.subscribe_private();
    std::printf("connected to Lighter %s\n", mainnet ? "mainnet" : "testnet");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!engine.book(btc.id).best_bid() && std::chrono::steady_clock::now() < deadline) engine.poll();
    const auto bid = engine.book(btc.id).best_bid();
    if (!bid) {
        std::fprintf(stderr, "error: no book after 15 s\n");
        engine.shutdown();
        return 1;
    }

    // 20% below the bid: it cannot fill.
    const double bid_usd = btc.to_double(*bid);
    const Price px = btc.price(std::floor(bid_usd * 0.8));
    const double px_usd = btc.to_double(px);
    const Qty qty{static_cast<int64_t>(std::ceil(12.0 / px_usd / btc.lot_size))};

    NewOrder order;
    // A cid as the engine makes them (ms * 1000): larger than Lighter's 2^48
    // limit, so this also tests the mapping to client_order_index.
    order.cid = static_cast<ClientOrderId>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count()) *
                1000;
    order.req = 1;
    order.inst = btc.id;
    order.side = Side::Buy;
    order.px = px;
    order.qty = qty;
    order.tif = Tif::PostOnly;

    std::printf("\n=== placing post-only BUY %.5f BTC @ %.1f (best bid %.1f), cid %llu ===\n",
                btc.to_double(qty), px_usd, bid_usd, static_cast<unsigned long long>(order.cid));
    lighter.send_new(order);
    run_for(engine, std::chrono::seconds(5));

    AmendOrder amend;
    amend.cid = order.cid;
    amend.req = 2;
    amend.inst = btc.id;
    amend.px = px - 1;
    std::printf("\n=== modifying cid %llu: price %.1f -> %.1f ===\n", static_cast<unsigned long long>(order.cid),
                px_usd, btc.to_double(amend.px));
    lighter.send_amend(amend);
    run_for(engine, std::chrono::seconds(5));

    std::printf("\n=== querying cid %llu ===\n", static_cast<unsigned long long>(order.cid));
    lighter.query_order(QueryOrder{.cid = order.cid, .inst = btc.id});
    run_for(engine, std::chrono::seconds(3));

    std::printf("\n=== cancelling cid %llu ===\n", static_cast<unsigned long long>(order.cid));
    CancelOrder cancel;
    cancel.cid = order.cid;
    cancel.req = 3;
    cancel.inst = btc.id;
    lighter.send_cancel(cancel);
    run_for(engine, std::chrono::seconds(5));

    std::printf("\n=== done: check on app.lighter.xyz that no order is left open ===\n");
    engine.shutdown();
    return 0;
}
