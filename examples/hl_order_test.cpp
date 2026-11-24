// Live test of Hyperliquid order entry: places ONE post-only BTC buy 20%
// below the best bid (so it cannot fill), moves it 1 USD lower, cancels it, and prints
// every raw message Hyperliquid sends back. Those messages are the examples
// for writing parse_post_response / parse_order_updates.
//
//   HL_ACCOUNT=0x<main account> HL_API_KEY=0x<API wallet private key> PEEWEE_DEBUG=1 ./hl_order_test <test|main>
//
// The order is worth about 12 USD (Hyperliquid's minimum is 10). If the
// program dies before cancelling, cancel it on app.hyperliquid.xyz.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "peewee/engine/engine.hpp"
#include "peewee/venues/hyperliquid.hpp"

using namespace peewee;

namespace {

// Keeps the engine running (and printing messages) for a while.
void run_for(Engine& engine, std::chrono::seconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) engine.poll();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || (std::strcmp(argv[1], "test") != 0 && std::strcmp(argv[1], "main") != 0)) {
        std::fprintf(stderr, "usage: HL_ACCOUNT=... HL_API_KEY=... PEEWEE_DEBUG=1 %s <test|main>\n", argv[0]);
        return 2;
    }
    const bool mainnet = std::strcmp(argv[1], "main") == 0;
    const char* account = std::getenv("HL_ACCOUNT");
    const char* key = std::getenv("HL_API_KEY");
    if (!account || !key) {
        std::fprintf(stderr, "error: set HL_ACCOUNT and HL_API_KEY\n");
        return 2;
    }

    Engine engine;
    HyperliquidConfig cfg;
    cfg.testnet = !mainnet;
    cfg.account_address = account;
    cfg.api_wallet_key = key;
    auto& hl = engine.add_venue<HyperliquidVenue>(cfg);
    // BTC: lot 0.00001 (szDecimals 5). Tick 1: at ~68000, 5 significant
    // digits leave no room for decimals.
    const auto& btc = engine.add_instrument(hl, "BTC", 1.0, 0.00001, 0.00001);

    try {
        engine.connect_all();  // Also checks that the key is an API wallet of HL_ACCOUNT.
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    hl.subscribe_book(btc.id, 20);
    std::printf("connected to Hyperliquid %s\n", mainnet ? "mainnet" : "testnet");

    // Wait for the book.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!engine.book(btc.id).best_bid() && std::chrono::steady_clock::now() < deadline) engine.poll();
    const auto bid = engine.book(btc.id).best_bid();
    if (!bid) {
        std::fprintf(stderr, "error: no book after 15 s\n");
        engine.shutdown();
        return 1;
    }

    // 20% below the bid, rounded down to a whole dollar: it cannot fill.
    const double bid_usd = btc.to_double(*bid);
    const Price px = btc.price(std::floor(bid_usd * 0.8));
    const double px_usd = btc.to_double(px);
    const Qty qty{static_cast<int64_t>(std::ceil(12.0 / px_usd / btc.lot_size))};

    NewOrder order;
    // A cid never used before on this account: Hyperliquid refuses duplicates.
    order.cid = static_cast<ClientOrderId>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    order.req = 1;
    order.inst = btc.id;
    order.side = Side::Buy;
    order.px = px;
    order.qty = qty;
    order.tif = Tif::PostOnly;

    std::printf("\n=== placing post-only BUY %.5f BTC @ %.0f (best bid %.0f), cid %llu ===\n",
                btc.to_double(qty), px_usd, bid_usd, static_cast<unsigned long long>(order.cid));
    hl.send_new(order);
    run_for(engine, std::chrono::seconds(5));

    // Move it one dollar lower (still far from the market): what does a modify produce?
    AmendOrder amend;
    amend.cid = order.cid;
    amend.req = 2;
    amend.inst = btc.id;
    amend.px = px - 1;
    std::printf("\n=== modifying cid %llu: price %.0f -> %.0f ===\n", static_cast<unsigned long long>(order.cid),
                px_usd, btc.to_double(amend.px));
    hl.send_amend(amend);
    run_for(engine, std::chrono::seconds(5));

    std::printf("\n=== cancelling cid %llu ===\n", static_cast<unsigned long long>(order.cid));
    CancelOrder cancel;
    cancel.cid = order.cid;
    cancel.req = 3;
    cancel.inst = btc.id;
    hl.send_cancel(cancel);
    run_for(engine, std::chrono::seconds(5));

    std::printf("\n=== done: check on app.hyperliquid.xyz that no order is left open ===\n");
    engine.shutdown();
    return 0;
}
