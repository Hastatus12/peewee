// Hyperliquid signing, checked against the official Python SDK.
//
// Every expected value below was produced by hyperliquid-python-sdk
// (msgpack.packb, action_hash, sign_l1_action) with the same inputs.
// The key is a FAKE test key, not tied to any account.

#include "check.hpp"
#include "hl_msgpack.hpp"
#include "hl_signer.hpp"

#include <string>

using namespace peewee;
using namespace peewee::hyperliquid;

namespace {

constexpr const char* kKey = "0x0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr const char* kCloid = "0x00000000000000000000000000000001";
constexpr const char* kVault = "0x1111111111111111111111111111111111111111";
constexpr int64_t kNonce = 1794611089218;

std::string hex(const std::string& bytes) {
    return to_hex(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()).substr(2);
}

std::string hex(const Hash& h) { return to_hex(h.data(), h.size()).substr(2); }

// {"a":..,"b":..,"p":..,"s":..,"r":false,"t":{"limit":{"tif":..}},"c":cloid}
void write_order(MsgpackWriter& w, uint64_t asset, bool buy, const char* px, const char* sz, const char* tif) {
    w.map(7);
    w.str("a"), w.uint(asset);
    w.str("b"), w.boolean(buy);
    w.str("p"), w.str(px);
    w.str("s"), w.str(sz);
    w.str("r"), w.boolean(false);
    w.str("t"), w.map(1), w.str("limit"), w.map(1), w.str("tif"), w.str(tif);
    w.str("c"), w.str(kCloid);
}

std::string order_action() {
    MsgpackWriter w;
    w.map(3);
    w.str("type"), w.str("order");
    w.str("orders"), w.array(1), write_order(w, 0, true, "65000", "0.001", "Alo");
    w.str("grouping"), w.str("na");
    return w.bytes();
}

std::string modify_action() {
    MsgpackWriter w;
    w.map(3);
    w.str("type"), w.str("modify");
    w.str("oid"), w.str(kCloid);
    w.str("order"), write_order(w, 3, false, "0.12345", "1000", "Gtc");
    return w.bytes();
}

std::string cancel_action() {
    MsgpackWriter w;
    w.map(2);
    w.str("type"), w.str("cancelByCloid");
    w.str("cancels"), w.array(1), w.map(2), w.str("asset"), w.uint(159), w.str("cloid"), w.str(kCloid);
    return w.bytes();
}

struct Expected {
    bool mainnet;
    bool vault;
    const char* hash;
    const char* r;
    const char* s;
    int v;
};

void check_signatures(const std::string& action, const Expected (&cases)[4]) {
    const Signer main_signer(kKey, true);
    const Signer test_signer(kKey, false);
    for (const Expected& c : cases) {
        const char* vault = c.vault ? kVault : "";
        CHECK_EQ(hex(action_hash(action, kNonce, vault)), std::string(c.hash));
        const Signature sig = (c.mainnet ? main_signer : test_signer).sign_l1_action(action, kNonce, vault);
        CHECK_EQ(sig.r, "0x" + std::string(c.r));
        CHECK_EQ(sig.s, "0x" + std::string(c.s));
        CHECK_EQ(sig.v, c.v);
    }
}

}  // namespace

TEST(keccak256_matches_reference) {
    CHECK_EQ(hex(keccak256("")), "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
    CHECK_EQ(hex(keccak256("abc")), "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");
    // 512 bytes: several 136-byte blocks.
    std::string long_input;
    for (int i = 0; i < 512; ++i) long_input += static_cast<char>(i % 256);
    CHECK_EQ(hex(keccak256(long_input)), "f55ba327291604f0e5be6651752398b7be2331aad65f5763ce067df95cc13be1");
}

TEST(signer_derives_address) {
    CHECK_EQ(Signer(kKey, true).address(), "0xfcad0b19bb29d4674531d6f115237e16afce377c");
}

TEST(signer_rejects_bad_keys) {
    bool threw = false;
    try {
        Signer("0x1234", true);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(msgpack_matches_python) {
    CHECK_EQ(hex(order_action()),
             "83a474797065a56f72646572a66f72646572739187a16100a162c3a170a53635303030a173a5302e303031a172c2a17481a56c"
             "696d697481a3746966a3416c6fa163d92230783030303030303030303030303030303030303030303030303030303030303031a8"
             "67726f7570696e67a26e61");
    CHECK_EQ(hex(modify_action()),
             "83a474797065a66d6f64696679a36f6964d92230783030303030303030303030303030303030303030303030303030303030303031"
             "a56f7264657287a16103a162c2a170a7302e3132333435a173a431303030a172c2a17481a56c696d697481a3746966a3477463a1"
             "63d92230783030303030303030303030303030303030303030303030303030303030303031");
    CHECK_EQ(hex(cancel_action()),
             "82a474797065ad63616e63656c4279436c6f6964a763616e63656c739182a56173736574cc9fa5636c6f6964"
             "d92230783030303030303030303030303030303030303030303030303030303030303031");
}

TEST(order_signature_matches_python) {
    const Expected cases[4] = {
        {true, false, "a20bd98bedaa59c654ff31dfc52f456f830ba1013c70630067a25a0c995bf6f9",
         "630815a2ad8229fa59fc541158e3871c4cc845906f96d544f407039131318f0a",
         "2ba5a56c107181f1630248aea5cc75d41873c108ca8dc22f911aa04c5c3006b7", 28},
        {true, true, "e579ebe9dd3745a243984c5f9b4729e9bdb4249e5bf098824d1896727e461606",
         "fafc0bebd542e98998b1b102bb51ec8ff646c4c976f897cfdbd735bad4a6b152",
         "37d14bfa94564efe523f27e5ee83b7f6772b3b3ca7ab05b63a68e2caa91379a2", 28},
        {false, false, "a20bd98bedaa59c654ff31dfc52f456f830ba1013c70630067a25a0c995bf6f9",
         "c1569be8c6d143eec32db6f0860407f11cfa41a0a971904843357e326450248d",
         "7788f1a25d1f580cb9f79c540ec574d9a579e30f02f5d70089b10c2cbde3b8af", 27},
        {false, true, "e579ebe9dd3745a243984c5f9b4729e9bdb4249e5bf098824d1896727e461606",
         "d7e9c3e9d54324f274d26d3aa22aa406394b1fb441dc4b8eaaca0147991418f2",
         "3410dd529b61666a165ed17919f830a4228c2f6a42e439332c64058c4f780847", 28},
    };
    check_signatures(order_action(), cases);
}

TEST(modify_signature_matches_python) {
    const Expected cases[4] = {
        {true, false, "b148d5a8705820725ce71f1c245d7cb5151b34cafeb27c2d3adb8a7883ba51e1",
         "5f87b003c08f1ed578dfbd83fead6de1de26d3e58d9ee1eac453135b0ff143a4",
         "438b840cd31d2a16aa1669e184fc4aeeca35a865ce894bc5362f346229cc6ac2", 27},
        {true, true, "3b859b8170895541e86f5a0657ab71f13c0091ee7622d32f66f7c869189107ba",
         "1e67f77c32e89b36fa198f7be196628efd430a374c4b24044484e9d4698ff4f0",
         "3309b8c6d1f1324afe2c3f716eaa394d4d3b7b5f5dd6dcc6c806068fba846284", 27},
        {false, false, "b148d5a8705820725ce71f1c245d7cb5151b34cafeb27c2d3adb8a7883ba51e1",
         "0b46574b5e825485fb7f00e00fcdea7595425a583fdea050c500f64d5c2629dc",
         "7997cfbfeee0626f882ae8aeaa70b5cbdb2a90cacbd6a17b58cc2e0413dd58f0", 27},
        {false, true, "3b859b8170895541e86f5a0657ab71f13c0091ee7622d32f66f7c869189107ba",
         "65c9bf6a1645a39ee563c8b43f6416ef5d53e402d233d6a3aec3e824af60a218",
         "70c2ae99137ea81a0fe5701fa3bef35c2d83d918aa8883513861b60fe96f0310", 27},
    };
    check_signatures(modify_action(), cases);
}

TEST(cancel_signature_matches_python) {
    const Expected cases[4] = {
        {true, false, "ce65a9c0dba78b42a9623df1f93f62029c12357f71866f55e0a56d230cd113a7",
         "e12ee8eed0dd72d62258808369fe72c00ebbd67123c4e38f4923ca354e330342",
         "6f225a8f46d1f4c1b828967a2533ea86c38843b1181a30c4a6226da0d7779def", 27},
        {true, true, "92cb4ce0e8ad9de208c1c38d6bf598cf9884c2cfe07e7c3fcfddf0e7591622b9",
         "5c0f15976e688409f24601479283aeb409f3c2e3ed0d32ba09f4086676bf2717",
         "415de77c75ceff0748a9f421107f06f2b7659265845c4a2c59ab732a6d9b2d85", 27},
        {false, false, "ce65a9c0dba78b42a9623df1f93f62029c12357f71866f55e0a56d230cd113a7",
         "764ed7ab9475c79a9b0631318a8b4a1578c37904b86ba9b1e370bf2186d36f26",
         "7b277c06cfd2ac61aa356f88252e1490e3a3bef810cdfc17683e13bd09038af9", 28},
        {false, true, "92cb4ce0e8ad9de208c1c38d6bf598cf9884c2cfe07e7c3fcfddf0e7591622b9",
         "4c755c01763b9e2ab1ef1c480be7e6ffe81d77ac08a44fb8c5103edbac02759e",
         "1fb58202c0556532e9e8ed97bf74e36c3f48fcaa08e21d8eea8c1be237762a47", 27},
    };
    check_signatures(cancel_action(), cases);
}

TEST_MAIN()
