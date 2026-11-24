// Live test of the new order types on Hyperliquid. Nothing can fill:
//
//  1. a STOP-MARKET BUY that triggers only if BTC rises 20% (it waits out of
//     the book), moved 1 USD, then cancelled;
//  2. a MARKET BUY whose worst price is 20% below the bid: there is nothing
//     to buy at that price, so Hyperliquid must cancel it at once (this
//     checks the price protection of market orders).
//
// Every raw message is printed: they are the examples for the trigger
// statuses in parse_order_updates.
//
//   HL_ACCOUNT=0x<main account> HL_API_KEY=0x<API wallet private key> PEEWEE_DEBUG=1 ./hl_trigger_test main
//
// Each order is worth about 12 USD. If the program dies before cancelling,
// cancel the stop on app.hyperliquid.xyz ("Open Orders").

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

// A cid never used before on this account: Hyperliquid refuses duplicates.
ClientOrderId fresh_cid() {
    return static_cast<ClientOrderId>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
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
    // BTC: lot 0.00001 (szDecimals 5), tick 1 (see hl_order_test.cpp).
    const auto& btc = engine.add_instrument(hl, "BTC", 1.0, 0.00001, 0.00001);

    try {
        engine.connect_all();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    hl.subscribe_book(btc.id, 20);
    hl.subscribe_private();
    std::printf("connected to Hyperliquid %s\n", mainnet ? "mainnet" : "testnet");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!engine.book(btc.id).best_bid() && std::chrono::steady_clock::now() < deadline) engine.poll();
    const auto bid = engine.book(btc.id).best_bid();
    if (!bid) {
        std::fprintf(stderr, "error: no book after 15 s\n");
        engine.shutdown();
        return 1;
    }
    const double bid_usd = btc.to_double(*bid);

    // --- 1. Stop-market buy, triggers only 20% above the market ---
    const Price trigger = btc.price(std::ceil(bid_usd * 1.2));
    const Price worst = btc.price(std::ceil(bid_usd * 1.21));
    NewOrder stop;
    stop.cid = fresh_cid();
    stop.req = 1;
    stop.inst = btc.id;
    stop.side = Side::Buy;
    stop.type = OrderType::StopMarket;
    stop.trigger_px = trigger;
    stop.px = worst;
    stop.qty = Qty{static_cast<int64_t>(std::ceil(12.0 / btc.to_double(trigger) / btc.lot_size))};

    std::printf("\n=== STOP-MARKET BUY %.5f BTC: trigger %.0f, worst %.0f (best bid %.0f), cid %llu ===\n",
                btc.to_double(stop.qty), btc.to_double(trigger), btc.to_double(worst), bid_usd,
                static_cast<unsigned long long>(stop.cid));
    hl.send_new(stop);
    run_for(engine, std::chrono::seconds(5));

    AmendOrder amend;
    amend.cid = stop.cid;
    amend.req = 2;
    amend.inst = btc.id;
    amend.px = worst + 1;
    std::printf("\n=== modifying the stop: worst price %.0f -> %.0f ===\n", btc.to_double(worst),
                btc.to_double(amend.px));
    hl.send_amend(amend);
    run_for(engine, std::chrono::seconds(5));

    std::printf("\n=== cancelling the stop ===\n");
    CancelOrder cancel;
    cancel.cid = stop.cid;
    cancel.req = 3;
    cancel.inst = btc.id;
    hl.send_cancel(cancel);
    run_for(engine, std::chrono::seconds(5));

    // --- 2. Market buy that may not pay more than 80% of the bid ---
    NewOrder market;
    market.cid = fresh_cid();
    market.req = 4;
    market.inst = btc.id;
    market.side = Side::Buy;
    market.type = OrderType::Market;
    market.px = btc.price(std::floor(bid_usd * 0.8));
    market.qty = Qty{static_cast<int64_t>(std::ceil(12.0 / btc.to_double(market.px) / btc.lot_size))};
    std::printf("\n=== MARKET BUY %.5f BTC, worst price %.0f (below the market: must NOT fill), cid %llu ===\n",
                btc.to_double(market.qty), btc.to_double(market.px), static_cast<unsigned long long>(market.cid));
    hl.send_new(market);
    run_for(engine, std::chrono::seconds(5));

    std::printf("\n=== done: check on app.hyperliquid.xyz that no order is open and no trade happened ===\n");
    engine.shutdown();
    return 0;
}
