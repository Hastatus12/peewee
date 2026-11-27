// Connects to Lighter (testnet by default) and prints the best bid/ask of
// BTC once a second. Needs no key: public data only.
//
//   ./lighter_book            testnet, 10 seconds
//   ./lighter_book main 30    mainnet, 30 seconds

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "peewee/engine/engine.hpp"
#include "peewee/venues/lighter.hpp"

using namespace peewee;

int main(int argc, char** argv) {
    const bool mainnet = argc > 1 && std::strcmp(argv[1], "main") == 0;
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 10;

    Engine engine;
    LighterConfig cfg;
    cfg.testnet = !mainnet;
    auto& lighter = engine.add_venue<LighterVenue>(cfg);

    try {
        // Tick and lot come from Lighter (they differ between mainnet and testnet).
        const auto spec = lighter.fetch_instrument("BTC");
        const auto& btc = engine.add_instrument(lighter, "BTC", spec.tick_size, spec.lot_size, spec.min_qty);
        engine.connect_all();
        lighter.subscribe_book(btc.id, 0);
        std::printf("connected to Lighter %s\n", mainnet ? "mainnet" : "testnet");

        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        auto next_print = std::chrono::steady_clock::now();
        size_t events = 0;
        while (std::chrono::steady_clock::now() < end) {
            events += engine.poll();
            if (std::chrono::steady_clock::now() < next_print) continue;
            next_print += std::chrono::seconds(1);

            const OrderBook& book = engine.book(btc.id);
            const auto bid = book.best_bid();
            const auto ask = book.best_ask();
            if (!book.valid() || !bid || !ask) {
                std::printf("waiting for the book... (%zu events)\n", events);
                continue;
            }
            std::printf("bid %.1f  ask %.1f  spread %lld tick  depth %zu/%zu  (%zu events)\n", btc.to_double(*bid),
                        btc.to_double(*ask), static_cast<long long>(*ask - *bid), book.depth(Side::Buy),
                        book.depth(Side::Sell), events);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    engine.shutdown();
    return 0;
}
