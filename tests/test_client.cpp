// The simple interface (Exchange, OrderHandle), driven by the simulator with
// the real clock and the engine on its background thread.

#include "check.hpp"
#include "peewee/client/exchange.hpp"
#include "peewee/engine/engine.hpp"
#include "peewee/venues/sim.hpp"

using namespace peewee;
using namespace std::chrono_literals;

namespace {

// An Exchange over a simulator. Book: bid 100.0 x 1 / ask 100.5 x 1.
struct SimExchange {
    SimExchange() {
        auto engine = std::make_unique<Engine>();
        sim = &engine->add_venue<SimVenue>(engine->clock());
        btc = &engine->add_instrument(*sim, "BTC", 0.1, 0.001, 0.001);
        sim->set_book(btc->id, {{btc->price(100.0), btc->qty(1)}}, {{btc->price(100.5), btc->qty(1)}});
        ex.emplace(std::move(engine), *sim);
    }

    // Runs on the engine thread, where the simulator lives.
    void on_engine(std::function<void(SimVenue&, const Instrument&)> fn) {
        ex->post([this, fn = std::move(fn)](Engine&) { fn(*sim, *btc); });
    }

    SimVenue* sim = nullptr;
    const Instrument* btc = nullptr;
    std::optional<Exchange> ex;
};

}  // namespace

TEST(market_order_fills_and_reports) {
    SimExchange t;
    auto order = t.ex->market("BTC", Side::Buy, 0.01);
    CHECK(order.wait_for(2s));
    CHECK_EQ(order.state(), OrderHandle::State::Filled);
    CHECK(std::abs(order.filled() - 0.01) < 1e-9);
    CHECK(std::abs(order.avg_price() - 100.5) < 1e-9);  // At the ask.
    CHECK_EQ(std::string(order.state_text()), "filled");
}

TEST(market_order_worst_price_follows_the_current_ask) {
    SimExchange t;
    // The ask moves to 105.0 before the order: the worst price is computed
    // from the book at that moment, not from an old price.
    t.on_engine([](SimVenue& sim, const Instrument& i) {
        sim.set_book(i.id, {{i.price(100.0), i.qty(1)}}, {{i.price(105.0), i.qty(1)}});
    });
    auto order = t.ex->market("BTC", Side::Buy, 0.01, {.max_move_pct = 0.1});
    // Worst price 105.0 + 0.1% = 105.1: fills at 105.0.
    CHECK(order.wait_for(2s));
    CHECK_EQ(order.state(), OrderHandle::State::Filled);
}

TEST(limit_order_rests_then_fills) {
    SimExchange t;
    auto order = t.ex->limit("BTC", Side::Buy, 0.01, 100.0, {.tif = Tif::PostOnly});
    CHECK(!order.wait_for(100ms));  // Resting in the book.
    CHECK_EQ(order.state(), OrderHandle::State::Working);

    t.on_engine([](SimVenue& sim, const Instrument& i) { sim.trade(i.id, Side::Sell, i.price(100.0), i.qty(0.01)); });
    CHECK(order.wait_for(2s));
    CHECK_EQ(order.state(), OrderHandle::State::Filled);
    CHECK(std::abs(order.avg_price() - 100.0) < 1e-9);
}

TEST(cancel_a_resting_order) {
    SimExchange t;
    auto order = t.ex->limit("BTC", Side::Buy, 0.01, 99.0);
    CHECK(!order.wait_for(100ms));
    order.cancel();
    CHECK(order.wait_for(2s));
    CHECK_EQ(order.state(), OrderHandle::State::Canceled);
}

TEST(rejected_order_carries_the_reason) {
    SimExchange t;
    t.on_engine([](SimVenue& sim, const Instrument&) {
        sim.reject_next(RequestKind::New, RejectReason::InsufficientBalance);
    });
    auto order = t.ex->limit("BTC", Side::Buy, 0.01, 99.0);
    CHECK(order.wait_for(2s));
    CHECK_EQ(order.state(), OrderHandle::State::Rejected);
    CHECK(!order.error().empty());
}

TEST(chase_follows_and_fills) {
    SimExchange t;
    auto order = t.ex->chase("BTC", Side::Buy, 0.01, {.max_move_pct = 1.0, .reprice_every = 10ms});
    CHECK(!order.wait_for(100ms));

    // The bid rises: the chase follows; then a seller hits it.
    t.on_engine([](SimVenue& sim, const Instrument& i) { sim.update_book(i.id, {{i.price(100.2), i.qty(1)}}, {}); });
    std::this_thread::sleep_for(200ms);
    t.on_engine([](SimVenue& sim, const Instrument& i) { sim.trade(i.id, Side::Sell, i.price(100.2), i.qty(0.01)); });
    CHECK(order.wait_for(2s));
    CHECK_EQ(order.state(), OrderHandle::State::Filled);
    CHECK(std::abs(order.avg_price() - 100.2) < 1e-9);
}

TEST(unknown_symbol_and_tiny_quantity_throw) {
    SimExchange t;
    bool unknown = false;
    try {
        t.ex->market("ETH", Side::Buy, 0.01);
    } catch (const std::invalid_argument&) {
        unknown = true;
    }
    CHECK(unknown);

    bool tiny = false;
    try {
        t.ex->limit("BTC", Side::Buy, 0.0001, 100.0);  // Lot is 0.001.
    } catch (const std::invalid_argument&) {
        tiny = true;
    }
    CHECK(tiny);
}

TEST(destroying_the_exchange_cancels_working_orders) {
    SimExchange t;
    auto order = t.ex->limit("BTC", Side::Buy, 0.01, 99.0);
    CHECK(!order.wait_for(100ms));
    t.ex.reset();  // Destroy the Exchange.
    CHECK(order.done());
    CHECK_EQ(order.state(), OrderHandle::State::Canceled);
    order.cancel();  // Safe after the Exchange is gone: does nothing.
}

TEST_MAIN()
