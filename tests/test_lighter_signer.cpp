// Lighter signing, checked against the official Go code.
//
// Every expected value below was produced by lighter-go (types/txtypes) and
// poseidon_crypto v0.0.15 with the same inputs; signatures use a FIXED
// Schnorr nonce k (schnorr.SchnorrSignHashedMessage2) so they are
// reproducible. The key is a FAKE test key (bytes 01..28), tied to no account.

#include "check.hpp"
#include "lighter_crypto.hpp"
#include "lighter_signer.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using namespace peewee::lighter;

namespace {

constexpr const char* kKey = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728";

std::array<uint8_t, 40> bytes_from(uint8_t first) {
    std::array<uint8_t, 40> b{};
    for (size_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint8_t>(first + i);
    return b;
}

template <size_t N>
std::string hex(const std::array<uint8_t, N>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (uint8_t c : bytes) {
        out += kDigits[c >> 4];
        out += kDigits[c & 0xf];
    }
    return out;
}

// The Go generator's k: bytes 100..139, reduced modulo n.
Scalar fixed_k() { return scalar_from_bytes(bytes_from(100)); }

Signer test_signer(uint32_t chain_id) { return Signer(kKey, chain_id, 12345, 3, fixed_k); }

constexpr TxHeader kHeader{.expired_at = 1700000600000, .nonce = 7};

}  // namespace

TEST(poseidon2_matches_go) {
    const std::vector<std::pair<int, const char*>> cases = {
        {0, "00000000000000000000000000000000000000000000000000000000000000000000000000000000"},
        {1, "dbc24a6b2b8521675bdbc3a045045f78aac50e0b746624771953f28af0bc7eceda549a077dbd5e31"},
        {7, "dd85fa444bd1ebdb4572842583f9b229c8c747d48815697bba5c9e32dec71b4ee34e15a6b41f7b2a"},
        {8, "3d34119729533099cf1859ace0140479a30ca4cb077efcd37fe6ca00d942b76b5e2db386fd348ae7"},
        {9, "f2bcd4dfaf1b32e188f8f7cf0434dc761d00f728496fa30256908e22ef201a5257d44fd4ba63506f"},
        {16, "876231af5826f1295f05a6c1f53d8754589c4f4d5b3a58eef8fb53b656ecf08fd4ba10711c19aa23"},
        {20, "2d409e9906d03ffabccf868094760776c88174a80cb19c9f87313a8f0d374bfed42de1f455af85eb"},
    };
    for (const auto& [n, expected] : cases) {
        std::vector<uint64_t> in;
        for (int i = 0; i < n; ++i) in.push_back(static_cast<uint64_t>(i + 1));  // 1, 2, ..., n
        CHECK_EQ(hex(to_bytes(hash_to_fp5(in))), expected);
    }
    // Values near p: the field reduction paths.
    const std::vector<uint64_t> big = {0xffffffff00000000ull, 0xfffffffeffffffffull, 0, 1ull << 63};
    CHECK_EQ(hex(to_bytes(hash_to_fp5(big))),
             "da2af691323700ffded3a739f3f4a0546decac67a436968af21ae97a570babeebe7eeb4f0c643b1c");
}

TEST(fp5_arithmetic_matches_go) {
    const Fp5 a = {1, 2, 3, 4, 5};
    const Fp5 b = {0xffffffff00000000ull, 7, 0, 123456789, 1ull << 62};
    CHECK((fp5_mul(a, b) == Fp5{9223372042260854308ull, 4611686028498803967ull, 14736753731ull,
                                13835058068290522403ull, 4611686018674301505ull}));
    CHECK((fp5_mul(b, b) == Fp5{9223372079804448759ull, 45724736250571549ull, 10018736341358365842ull,
                                18446744068362364375ull, 9223372034288203559ull}));
    CHECK((fp5_inverse(b) == Fp5{7677715241205400668ull, 2594660183669636114ull, 10875030766016450597ull,
                                 4512310027141751059ull, 4233898926752413539ull}));
    CHECK((fp5_mul(b, fp5_inverse(b)) == Fp5{1, 0, 0, 0, 0}));
}

TEST(scalars_match_go) {
    const Scalar sk = scalar_from_bytes(bytes_from(1));
    const Scalar k = fixed_k();
    CHECK_EQ(hex(to_bytes_scalar(sk)), kKey);  // Already < n: unchanged.
    CHECK_EQ(hex(to_bytes_scalar(k)), "8365dad2d18f5a83cfcc49983615ea8a3a6fbea791797afb657d7e7f8f8182037d8586078b898a0b");
    std::array<uint8_t, 40> ff{};
    ff.fill(0xff);  // 2^320 - 1: reduced twice.
    CHECK_EQ(hex(to_bytes_scalar(scalar_from_bytes(ff))),
             "3d00e8d6d24ce02fc6beb6518c47ef2e8cf38f6032000000d3ffffff1d000000f1ffffff04000000");
    CHECK_EQ(hex(to_bytes_scalar(scalar_mul(sk, k))),
             "3ac3275b855f3a8f6e5b28b2d1943b13e38bb521a2b8f70e977b273194344be6bf32b7ea02719a3d");
    CHECK_EQ(hex(to_bytes_scalar(scalar_sub(sk, k))),
             "7e9c28313376ac84393dc173d6f82485d6a2546c839c9c1cb39c9c9c8d9c9c1ca49c9c1c9a9c9c1c");
}

TEST(public_key_and_signature_match_go) {
    const Scalar sk = scalar_from_bytes(bytes_from(1));
    CHECK_EQ(hex(to_bytes(public_key(sk))),
             "710c8cd2201061fa5570d20852d90ddabd6c496825b639d6c6486ecb1f859b63f237dee54f3f42b4");

    const std::vector<uint64_t> msg = {10, 11, 12};
    const Fp5 hashed = hash_to_fp5(msg);
    CHECK_EQ(hex(to_bytes(hashed)), "117e1ecdd45870ab6f85cd6e9ff1441f0d08665c126d60f9022df3c88c0ee2b2aae1f8c484ed2abc");
    CHECK_EQ(hex(schnorr_sign(hashed, sk, fixed_k())),
             "c62f4ec949bc785f0d54ee22ccf7ac1b1e78e6ce9e7f49adb7c2886dc5cba80f8d3a0cafa0a985212eaabf8ade8a28bc"
             "22bc06caad4ff9b4fc6f5e26ff890503b6f4c74c8b1b7ad8306c4737eea3d01d");
}

TEST(signer_public_key_accepts_0x_prefix) {
    const Signer a(kKey, kMainnetChainId, 1, 0);
    const Signer b(std::string("0x") + kKey, kMainnetChainId, 1, 0);
    CHECK_EQ(a.public_key_hex(), "710c8cd2201061fa5570d20852d90ddabd6c496825b639d6c6486ecb1f859b63f237dee54f3f42b4");
    CHECK_EQ(b.public_key_hex(), a.public_key_hex());
}

TEST(signer_rejects_malformed_keys) {
    bool threw = false;
    try {
        Signer s("0x1234", kMainnetChainId, 1, 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(create_order_matches_go) {
    CreateOrderTx tx{.market_index = 1, .client_order_index = 42, .base_amount = 2000, .price = 6500012,
                     .is_ask = 0, .type = 0, .time_in_force = 2, .reduce_only = 0, .trigger_price = 0,
                     .order_expiry = 1800000000000};
    const Signer s = test_signer(kTestnetChainId);
    CHECK_EQ(hex(to_bytes(s.hash_create_order(tx, kHeader))),
             "3c72d85362bf2d447a30d2ec7bca461fded088f0ba90f4c2ee0d1da4cd05646002345f8779844e7c");
    CHECK_EQ(s.sign_create_order(tx, kHeader),
             R"({"AccountIndex":12345,"ApiKeyIndex":3,"MarketIndex":1,"ClientOrderIndex":42,"BaseAmount":2000,)"
             R"("Price":6500012,"IsAsk":0,"Type":0,"TimeInForce":2,"ReduceOnly":0,"TriggerPrice":0,)"
             R"("OrderExpiry":1800000000000,"ExpiredAt":1700000600000,"Nonce":7,)"
             R"("Sig":"KZhGNGHRMQTqDvjdsm+wuWmNFIAv+R8eYC4TVwGJQkE81Nd1p/8jHwH7ik5ILCl3raEF0gOQqKzBuLlcwflajXooOCo97XQ0vBzw7ebtqy4=",)"
             R"("L2TxAttributes":null})");
}

TEST(stop_loss_order_on_mainnet_matches_go) {
    // Largest client order index, reduce-only sell, trigger price, chain 304.
    CreateOrderTx tx{.market_index = 0, .client_order_index = 281474976710655, .base_amount = 1, .price = 1,
                     .is_ask = 1, .type = 2, .time_in_force = 0, .reduce_only = 1, .trigger_price = 300000,
                     .order_expiry = 1800000000000};
    const Signer s = test_signer(kMainnetChainId);
    const TxHeader h{.expired_at = 1700000600000, .nonce = 8};
    CHECK_EQ(hex(to_bytes(s.hash_create_order(tx, h))),
             "6e9138e2790fc31d2919ac06de45be4eb71b59e0b3f8670ae8e717b8dfa29afed98e319cc66a6eec");
    CHECK_EQ(s.sign_create_order(tx, h),
             R"({"AccountIndex":12345,"ApiKeyIndex":3,"MarketIndex":0,"ClientOrderIndex":281474976710655,)"
             R"("BaseAmount":1,"Price":1,"IsAsk":1,"Type":2,"TimeInForce":0,"ReduceOnly":1,"TriggerPrice":300000,)"
             R"("OrderExpiry":1800000000000,"ExpiredAt":1700000600000,"Nonce":8,)"
             R"("Sig":"O7EefrWgvGDMpPpcUeahdXTA51W8aXThUOBDK/Q2Z37qT2zfEyoYWqfpPjJyxA/a1hlUeAf9GvYR6YguSTxUWpPPZq76RgU8sad/ZC2MS28=",)"
             R"("L2TxAttributes":null})");
}

TEST(cancel_order_matches_go) {
    const Signer s = test_signer(kTestnetChainId);
    const TxHeader h{.expired_at = 1700000600000, .nonce = 9};
    const CancelOrderTx tx{.market_index = 1, .index = 42};
    CHECK_EQ(hex(to_bytes(s.hash_cancel_order(tx, h))),
             "11e75ae20f82ea8802aed69efdd604cc0b95148ba2e2747fd248fd1c03fc0cbbe1e17174f9c375a9");
    CHECK_EQ(s.sign_cancel_order(tx, h),
             R"({"AccountIndex":12345,"ApiKeyIndex":3,"MarketIndex":1,"Index":42,"ExpiredAt":1700000600000,)"
             R"("Nonce":9,"Sig":"IseUG/wqUuc2yBALrkcLg3r8W0Co8kDZMbNr2Ko8TUbXa/hTiNBTNzzupXxsY3upMR8f3DpLWfrCn738Jw3U+iH816mC5IH48XzS2qkyAB0=",)"
             R"("L2TxAttributes":null})");
}

TEST(modify_order_matches_go) {
    const Signer s = test_signer(kTestnetChainId);
    const TxHeader h{.expired_at = 1700000600000, .nonce = 10};
    const ModifyOrderTx tx{.market_index = 1, .index = 42, .base_amount = 3000, .price = 6500100, .trigger_price = 0};
    CHECK_EQ(hex(to_bytes(s.hash_modify_order(tx, h))),
             "feec9fd0940462bca5bb7e6c969840df160dafd6a56a2c55c561083cc56aa0598ed2f0e98353e06d");
    CHECK_EQ(s.sign_modify_order(tx, h),
             R"({"AccountIndex":12345,"ApiKeyIndex":3,"MarketIndex":1,"Index":42,"BaseAmount":3000,"Price":6500100,)"
             R"("TriggerPrice":0,"ExpiredAt":1700000600000,"Nonce":10,)"
             R"("Sig":"Q+9SBgwKiaMq8GwkSKVg9T9Yvjwl+R2l0dQvyeRsmadEIQfm+kgMQ5oEhIYAjoa3lEenyTlv71bH0n4vqCwwBbWQTSd7c7fTAMXbtdh/Q0g=",)"
             R"("L2TxAttributes":null})");
}

TEST(auth_token_matches_go) {
    const Signer s = test_signer(kTestnetChainId);
    CHECK_EQ(s.auth_token(1700025200),
             "1700025200:12345:3:c86fdf0b2ddebb14a22e720ba44b2762acb75a5292b77fbcc4e9a1d1a1f596860afa0dc42fc6982c"
             "616579e294412f7035408c575a1175de285b94c0d1164ae27eb61c2c48388262aaeabd7cf94b881a");
}

TEST(random_signatures_differ) {
    // The production nonce is random: two signatures of the same tx differ,
    // and a reused k would leak the key.
    const Signer s(kKey, kTestnetChainId, 12345, 3);
    const CancelOrderTx tx{.market_index = 1, .index = 42};
    CHECK(s.sign_cancel_order(tx, kHeader) != s.sign_cancel_order(tx, kHeader));
    const Scalar k1 = random_scalar();
    const Scalar k2 = random_scalar();
    CHECK(k1 != k2);
}

TEST_MAIN()
