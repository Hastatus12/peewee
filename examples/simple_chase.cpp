// One chase order through the simple interface (Exchange, OrderHandle).
//
//   HL_ACCOUNT=0x... HL_API_KEY=0x...         ./simple_chase hl HYPE buy 0.2 [MAX_MOVE_PCT]
//   BYBIT_API_KEY=... BYBIT_API_SECRET=...    ./simple_chase bybit BTCPERP buy 0.001 [MAX_MOVE_PCT]
//
// MAINNET: real money. MAX_MOVE_PCT (default 1): buy at most this % above
// the ask seen at start, sell at least this % below the bid. The chase stops
// by itself after 5 minutes; Ctrl+C cancels it earlier.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "peewee/client/exchange.hpp"

using namespace peewee;
using namespace std::chrono_literals;

namespace {
std::atomic<bool> g_cancel{false};  // Set by Ctrl+C; a signal handler may only touch atomics.
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 5 || (std::strcmp(argv[1], "hl") != 0 && std::strcmp(argv[1], "bybit") != 0) ||
        (std::strcmp(argv[3], "buy") != 0 && std::strcmp(argv[3], "sell") != 0)) {
        std::fprintf(stderr, "usage: %s <hl|bybit> SYMBOL <buy|sell> QTY [MAX_MOVE_PCT]\n", argv[0]);
        return 2;
    }
    const bool hl = std::strcmp(argv[1], "hl") == 0;
    const char* symbol = argv[2];
    const Side side = std::strcmp(argv[3], "buy") == 0 ? Side::Buy : Side::Sell;
    const double qty = std::atof(argv[4]);
    const double max_move = argc > 5 ? std::atof(argv[5]) : 1.0;

    try {
        auto ex = hl ? Exchange::hyperliquid({symbol}) : Exchange::bybit({symbol});
        std::printf("connected. chase: %s %g %s, max move %g%%\n", argv[3], qty, symbol, max_move);

        auto order = ex.chase(symbol, side, qty, {.max_move_pct = max_move, .max_duration = 300s});
        std::signal(SIGINT, [](int) { g_cancel = true; });

        double last_filled = 0.0;
        while (!order.wait_for(200ms)) {
            if (g_cancel.exchange(false)) {
                std::printf("Ctrl+C: cancelling\n");
                order.cancel();
            }
            if (order.filled() != last_filled) {
                last_filled = order.filled();
                std::printf("filled so far: %g at %g\n", last_filled, order.avg_price());
            }
        }
        std::printf("result: %s, filled %g at %g\n", order.state_text(), order.filled(), order.avg_price());
        if (!order.error().empty()) std::printf("error: %s\n", order.error().c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;  // ~Exchange cancels anything still working, then disconnects.
}
