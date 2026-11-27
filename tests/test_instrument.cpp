// Valid prices of an instrument with a significant-digits limit (Hyperliquid).

#include "check.hpp"
#include "peewee/core/instrument.hpp"

using namespace peewee;

namespace {

Instrument make(double tick, int sig_figs) {
    Instrument i;
    i.symbol = "TEST";
    i.tick_size = tick;
    i.lot_size = 0.01;
    i.price_sig_figs = sig_figs;
    return i;
}

}  // namespace

TEST(without_a_limit_every_tick_is_valid) {
    const Instrument i = make(0.1, 0);
    CHECK(i.is_valid(i.price(12345.6)));
    CHECK_EQ(i.step_up(i.price(100.0)), i.price(100.1));
    CHECK_EQ(i.step_down(i.price(100.0)), i.price(99.9));
    CHECK_EQ(i.round_down(i.price(100.3)), i.price(100.3));
}

TEST(five_significant_digits) {
    const Instrument btc = make(0.1, 5);  // BTC on Hyperliquid: szDecimals 5, tick 0.1.
    CHECK(btc.is_valid(btc.price(1234.5)));
    CHECK(!btc.is_valid(btc.price(12345.6)));
    CHECK_EQ(btc.round_down(btc.price(12345.6)), btc.price(12345.0));
    CHECK_EQ(btc.round_up(btc.price(12345.6)), btc.price(12346.0));
    CHECK(btc.is_valid(btc.price(61000.0)));
}

TEST(whole_prices_are_always_valid) {
    const Instrument btc = make(0.1, 5);
    CHECK(btc.is_valid(btc.price(123456.0)));  // 6 digits, but a whole number.
    CHECK(!btc.is_valid(btc.price(123456.7)));
    CHECK_EQ(btc.round_down(btc.price(123456.7)), btc.price(123456.0));
    CHECK_EQ(btc.step_up(btc.price(123456.0)), btc.price(123457.0));
}

TEST(steps_follow_the_price_across_a_power_of_ten) {
    const Instrument hype = make(0.0001, 5);  // HYPE: szDecimals 2, tick 0.0001.
    // Below 10 the step is 0.0001, from 10 it is 0.001.
    CHECK_EQ(hype.step_up(hype.price(9.9999)), hype.price(10.0));
    CHECK_EQ(hype.step_up(hype.price(10.0)), hype.price(10.001));
    CHECK_EQ(hype.step_down(hype.price(10.0)), hype.price(9.9999));
    CHECK_EQ(hype.step_down(hype.price(10.001)), hype.price(10.0));
    CHECK_EQ(hype.round_up(hype.price(9.99995)), hype.price(10.0));  // 99999.5 ticks rounds to 100000
}

TEST(rounding_never_crosses_the_original_price) {
    const Instrument i = make(0.01, 5);
    for (int64_t t = 99'000; t <= 101'000; ++t) {
        const Price p{t * 10 + 7};  // Prices with a 7 in the last place: mostly invalid.
        CHECK(i.round_down(p) <= p);
        CHECK(i.round_up(p) >= p);
        CHECK(i.is_valid(i.round_down(p)));
        CHECK(i.is_valid(i.round_up(p)));
    }
}

TEST_MAIN()
