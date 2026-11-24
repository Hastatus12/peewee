// Real exchange example: one chase order on Bybit or Hyperliquid.
//
//   BYBIT_API_KEY=... BYBIT_API_SECRET=... ./chase_live bybit <test|main> <SYMBOL> <buy|sell> <QTY> <LIMIT> [options]
//   HL_ACCOUNT=0x... HL_API_KEY=0x...      ./chase_live hl    <test|main> <COIN>   <buy|sell> <QTY> <LIMIT> [options]
//
// Examples: ./chase_live bybit main BTCPERP buy 0.001 60000 --reprice-ms 500
//           ./chase_live hl main BTC buy 0.0002 60000 --max-seconds 120
//           ./chase_live hl main HYPE buy 0.2 --max-move 1%   (no LIMIT: computed)
// "main" trades REAL money. Tick and lot sizes are read from the exchange
// (on Hyperliquid the tick is computed for prices near LIMIT, or near the
// current price with --max-move). Hyperliquid orders must be worth at least 10 USD.
// LIMIT can be left out when --max-move is given.
// Options (ChaseParams, defaults in chase_order.hpp):
//   --max-move P%      no LIMIT: buy at most P% above the ask, sell at least P% below the bid
//   --reprice-ms N     minimum time between two amends
//   --offset N         ticks in front of the best price (0 = join it)
//   --max-reprices N   amends allowed in total
//   --max-seconds N    stop and cancel after N seconds

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "peewee/algo/chase_order.hpp"
#include "peewee/engine/engine.hpp"
#include "peewee/venues/bybit.hpp"
#include "peewee/venues/hyperliquid.hpp"

using namespace peewee;

namespace {
Engine* g_engine = nullptr;

int usage() {
    std::fprintf(stderr,
                 "usage: chase_live <bybit|hl> <test|main> <SYMBOL> <buy|sell> <QTY> [LIMIT] [options]\n"
                 "  e.g. chase_live bybit main BTCPERP buy 0.001 60000 --reprice-ms 500\n"
                 "       chase_live hl main BTC buy 0.0002 60000 --max-seconds 120\n"
                 "       chase_live hl main HYPE buy 0.2 --max-move 1%%\n"
                 "  'main' trades REAL money. Give LIMIT, or --max-move.\n"
                 "  bybit: BYBIT_API_KEY, BYBIT_API_SECRET; hl: HL_ACCOUNT, HL_API_KEY (environment)\n"
                 "options:\n"
                 "  --max-move P%%      without LIMIT: buy at most P%% above the ask,\n"
                 "                     sell at least P%% below the bid (e.g. 1%%)\n"
                 "  --reprice-ms N     minimum time between two amends (default 50)\n"
                 "  --offset N         ticks in front of the best price (default 0 = join it)\n"
                 "  --max-reprices N   amends allowed in total (default 10000)\n"
                 "  --max-seconds N    stop and cancel after N seconds (default: never)\n");
    return 2;
}

// Reads "--name value" pairs after the positional arguments. False on an
// unknown option or a missing / negative value.
bool parse_options(int argc, char** argv, int first, ChaseParams& p) {
    for (int i = first; i < argc; i += 2) {
        if (i + 1 >= argc) return false;
        const char* name = argv[i];
        if (std::strcmp(name, "--max-move") == 0) {
            // "1%" or "1": a percentage. More than 10% is almost surely a typo.
            const double pct = std::strtod(argv[i + 1], nullptr);
            if (pct <= 0.0 || pct > 10.0) return false;
            p.max_move_pct = pct;
            continue;
        }
        const long value = std::strtol(argv[i + 1], nullptr, 10);
        if (value < 0) return false;
        if (std::strcmp(name, "--reprice-ms") == 0) {
            p.min_reprice_interval = std::chrono::milliseconds(value);
        } else if (std::strcmp(name, "--offset") == 0) {
            p.offset_ticks = static_cast<int>(value);
        } else if (std::strcmp(name, "--max-reprices") == 0) {
            p.max_reprices = static_cast<int>(value);
        } else if (std::strcmp(name, "--max-seconds") == 0) {
            p.max_duration = std::chrono::seconds(value);
        } else {
            return false;
        }
    }
    return true;
}

// If the order is still open after the chase finished, cancel it and wait
// for Bybit to confirm (at most 10 s). False if it may still be open.
bool cancel_leftover(Engine& engine, ClientOrderId cid) {
    const Order* o = engine.oms().find(cid);
    if (!o || is_terminal(o->state)) return true;

    std::printf("order %llu is still open: cancelling it\n", static_cast<unsigned long long>(cid));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        o = engine.oms().find(cid);
        if (is_terminal(o->state)) {
            std::printf("order %llu is now %s\n", static_cast<unsigned long long>(cid), to_string(o->state));
            return true;
        }
        if (!o->has_pending()) engine.oms().send_cancel(cid);  // One request at a time.
        engine.poll();
    }
    std::fprintf(stderr, "WARNING: order %llu may still be open, check it on the website\n",
                 static_cast<unsigned long long>(cid));
    return false;
}
}  // namespace

int main(int argc, char** argv) {
    // Print each line at once even when piped (e.g. into tee), so these lines
    // and the library's log lines (stderr) come out in the right order.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    if (argc < 7) return usage();
    const bool hl = std::strcmp(argv[1], "hl") == 0;
    if (!hl && std::strcmp(argv[1], "bybit") != 0) return usage();
    const bool mainnet = std::strcmp(argv[2], "main") == 0;
    if (!mainnet && std::strcmp(argv[2], "test") != 0) return usage();
    const char* symbol = argv[3];
    const bool buy = std::strcmp(argv[4], "buy") == 0;
    if (!buy && std::strcmp(argv[4], "sell") != 0) return usage();
    const double qty = std::atof(argv[5]);
    // LIMIT is optional: after QTY comes either a number or the first option.
    const bool has_limit = argv[6][0] != '-';
    const double limit = has_limit ? std::atof(argv[6]) : 0.0;
    if (qty <= 0.0 || (has_limit && limit <= 0.0)) return usage();
    ChaseParams p;
    if (!parse_options(argc, argv, has_limit ? 7 : 6, p)) return usage();
    if (!has_limit && p.max_move_pct <= 0.0) {
        std::fprintf(stderr, "error: give a LIMIT or --max-move\n");
        return usage();
    }
    if (has_limit && p.max_move_pct > 0.0) {
        std::fprintf(stderr, "error: give a LIMIT or --max-move, not both\n");
        return usage();
    }

    Engine engine;
    g_engine = &engine;

    try {
        // The venue and the instrument's tick / lot, read from the exchange.
        IVenue* venue = nullptr;
        double tick = 0.0;
        double lot = 0.0;
        double min_qty = 0.0;
        if (hl) {
            const char* account = std::getenv("HL_ACCOUNT");
            const char* key = std::getenv("HL_API_KEY");
            HyperliquidConfig cfg;
            cfg.account_address = account ? account : "";
            cfg.api_wallet_key = key ? key : "";
            cfg.testnet = !mainnet;
            auto& v = engine.add_venue<HyperliquidVenue>(cfg);
            const double ref_px = has_limit ? limit : v.fetch_mid(symbol);
            const auto spec = v.fetch_instrument(symbol, ref_px);
            venue = &v;
            tick = spec.tick_size;
            lot = spec.lot_size;
            min_qty = spec.min_qty;
            if (qty * ref_px < 10.0) {
                std::fprintf(stderr, "error: %g x %g = %.2f USD, Hyperliquid needs at least 10 USD\n", qty, ref_px,
                             qty * ref_px);
                return 1;
            }
        } else {
            const char* key = std::getenv("BYBIT_API_KEY");
            const char* secret = std::getenv("BYBIT_API_SECRET");
            BybitConfig cfg;
            cfg.api_key = key ? key : "";
            cfg.api_secret = secret ? secret : "";
            cfg.testnet = !mainnet;
            auto& v = engine.add_venue<BybitVenue>(cfg);
            const auto spec = v.fetch_instrument(symbol);
            venue = &v;
            tick = spec.tick_size;
            lot = spec.lot_size;
            min_qty = spec.min_qty;
        }
        const auto& inst = engine.add_instrument(*venue, symbol, tick, lot, min_qty);
        std::printf("%s %s %s: tick %g, lot %g, min qty %g\n", hl ? "hyperliquid" : "bybit",
                    mainnet ? "MAINNET (real money)" : "testnet", symbol, tick, lot, min_qty);

        engine.connect_all();
        if (has_limit) {
            std::printf("connected; starting chase: %s %g %s, limit %g (Ctrl+C to stop)\n", buy ? "buy" : "sell",
                        qty, symbol, limit);
        } else {
            std::printf("connected; starting chase: %s %g %s, limit = %s %g%% (Ctrl+C to stop)\n",
                        buy ? "buy" : "sell", qty, symbol, buy ? "best ask +" : "best bid -", p.max_move_pct);
        }
        std::printf("reprice every >= %lld ms, offset %d ticks, max %d reprices, max duration %lld s\n",
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(p.min_reprice_interval).count()),
                    p.offset_ticks, p.max_reprices,
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(p.max_duration).count()));

        p.side = buy ? Side::Buy : Side::Sell;
        p.qty = inst.qty(qty);
        p.limit = has_limit ? inst.price(limit) : Price{};  // Price{}: the chase computes it (max_move_pct).
        const auto& chase = engine.start_algo<ChaseOrder>(*venue, inst, p);
        if (!has_limit) {
            // The chase sets its limit on the first valid book: show it.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (chase.limit().ticks == 0 && !chase.done() && std::chrono::steady_clock::now() < deadline) {
                engine.poll();
            }
            std::printf("limit computed: %g\n", inst.to_double(chase.limit()));
        }

        // Ctrl+C: ask algorithms to stop (they cancel their orders).
        // A signal handler may only do async-signal-safe work: request_stop_algos()
        // just sets an atomic flag, and the engine loop does the actual stopping.
        std::signal(SIGINT, [](int) { if (g_engine) g_engine->request_stop_algos(); });

        const auto started = std::chrono::steady_clock::now();
        engine.run();
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::printf("chase finished after %.1f s: %s, filled %g\n", seconds, to_string(chase.result()),
                    inst.to_double(chase.filled()));

        // Safety net: never leave a real order behind, whatever the chase did.
        if (!cancel_leftover(engine, chase.order_id())) return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
