// Live test of a stop order that REALLY triggers on Hyperliquid. It spends a
// few cents of fees:
//
//  1. a STOP-MARKET BUY of ~12 USD of BTC, triggering just above the market
//     (+0.02%), so a small rise of BTC triggers it;
//  2. waits up to 5 minutes for the trigger and the fill;
//  3. if it filled: closes the position at once with a reduce-only MARKET
//     SELL of the same quantity. If it did not trigger: cancels the stop.
//
// The orders go through the OMS (engine.oms()), so the program knows the
// order states, and every raw message is printed (PEEWEE_DEBUG=1).
//
//   HL_ACCOUNT=0x<main account> HL_API_KEY=0x<API wallet private key> PEEWEE_DEBUG=1 ./hl_stop_fill_test main
//
// If it dies halfway: on app.hyperliquid.xyz cancel the open stop and close
// the small BTC position, if any.

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

using Clock = std::chrono::steady_clock;

// Runs the engine until the order is closed (or the time is up).
bool wait_closed(Engine& engine, ClientOrderId cid, std::chrono::seconds max, const Instrument& inst) {
    const auto end = Clock::now() + max;
    auto next_print = Clock::now();
    while (Clock::now() < end) {
        engine.poll();
        const Order* o = engine.oms().find(cid);
        if (o && is_terminal(o->state)) return true;
        if (Clock::now() >= next_print) {
            const auto bid = engine.book(inst.id).best_bid();
            std::printf("... waiting: order %s, best bid %.0f\n", o ? to_string(o->state) : "?",
                        bid ? inst.to_double(*bid) : 0.0);
            next_print = Clock::now() + std::chrono::seconds(15);
        }
    }
    return false;
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

    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (!engine.book(btc.id).best_ask() && Clock::now() < deadline) engine.poll();
    const auto ask = engine.book(btc.id).best_ask();
    if (!ask) {
        std::fprintf(stderr, "error: no book after 15 s\n");
        engine.shutdown();
        return 1;
    }
    const double ask_usd = btc.to_double(*ask);

    // --- 1. Stop-market buy just above the market ---
    NewOrder stop;
    stop.inst = btc.id;
    stop.side = Side::Buy;
    stop.type = OrderType::StopMarket;
    stop.trigger_px = btc.price(std::ceil(ask_usd * 1.0002));
    stop.px = btc.price(std::ceil(ask_usd * 1.005));  // Worst price: at most +0.5%.
    stop.qty = Qty{static_cast<int64_t>(std::ceil(12.0 / ask_usd / btc.lot_size))};
    const ClientOrderId stop_cid = engine.oms().send_new(0, hl, stop);
    std::printf("\n=== STOP-MARKET BUY %.5f BTC: trigger %.0f, worst %.0f (best ask %.0f) ===\n",
                btc.to_double(stop.qty), btc.to_double(stop.trigger_px), btc.to_double(stop.px), ask_usd);

    // --- 2. Wait for trigger + fill ---
    wait_closed(engine, stop_cid, std::chrono::seconds(300), btc);
    const Order* o = engine.oms().find(stop_cid);
    const bool filled = o && o->state == OrderState::Filled;

    if (!filled) {
        // --- 3a. No trigger: cancel the stop ---
        std::printf("\n=== the stop did not fill (state %s): cancelling it ===\n", o ? to_string(o->state) : "?");
        engine.oms().send_cancel(stop_cid);
        wait_closed(engine, stop_cid, std::chrono::seconds(10), btc);
    } else {
        // --- 3b. Filled: close the position with a reduce-only market sell ---
        const auto bid = engine.book(btc.id).best_bid();
        NewOrder close;
        close.inst = btc.id;
        close.side = Side::Sell;
        close.type = OrderType::Market;
        close.px = btc.price(std::floor(btc.to_double(bid ? *bid : *ask) * 0.99));  // Worst: -1%.
        close.qty = stop.qty;
        close.reduce_only = true;
        std::printf("\n=== stop FILLED: closing with a reduce-only MARKET SELL %.5f BTC, worst %.0f ===\n",
                    btc.to_double(close.qty), btc.to_double(close.px));
        const ClientOrderId close_cid = engine.oms().send_new(0, hl, close);
        wait_closed(engine, close_cid, std::chrono::seconds(15), btc);
        const Order* c = engine.oms().find(close_cid);
        std::printf("=== close order: %s ===\n", c ? to_string(c->state) : "?");
    }

    // A few more seconds to print the last messages (fills).
    const auto end = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < end) engine.poll();

    std::printf("\n=== done: check on app.hyperliquid.xyz that no order and no BTC position are left ===\n");
    engine.shutdown();
    return 0;
}
