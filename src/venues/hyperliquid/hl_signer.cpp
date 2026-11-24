#include "hl_signer.hpp"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>

#include <cstring>
#include <stdexcept>
#include <vector>

// The ECDSA part (RFC 6979 nonce, recovery id, low-s) follows the approach of
// hyperliquid-cpp (https://github.com/charlitocrc/hyperliquid-cpp),
// MIT License, Copyright (c) 2025 Hyperliquid C++ SDK Contributors
// (full license text in THIRD_PARTY_NOTICES.md).
// It uses only BIGNUM/EC_POINT, not the EC_KEY API deprecated in OpenSSL 3.

namespace peewee::hyperliquid {

// ---------------------------------------------------------------------------
// Keccak-256
//
// Written here because OpenSSL only has it from 3.2 (Ubuntu 24.04 ships 3.0).
// Compact form of the reference Keccak-f[1600] permutation.
// ---------------------------------------------------------------------------

namespace {

constexpr uint64_t kRoundConstants[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};
constexpr int kRotations[24] = {1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};
constexpr int kLanes[24] = {10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};

uint64_t rotl(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }

void keccak_f(uint64_t st[25]) {
    for (int round = 0; round < 24; ++round) {
        uint64_t bc[5];
        // Theta
        for (int i = 0; i < 5; ++i) bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
        for (int i = 0; i < 5; ++i) {
            const uint64_t t = bc[(i + 4) % 5] ^ rotl(bc[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5) st[j + i] ^= t;
        }
        // Rho and Pi
        uint64_t t = st[1];
        for (int i = 0; i < 24; ++i) {
            const int j = kLanes[i];
            const uint64_t tmp = st[j];
            st[j] = rotl(t, kRotations[i]);
            t = tmp;
        }
        // Chi
        for (int j = 0; j < 25; j += 5) {
            for (int i = 0; i < 5; ++i) bc[i] = st[j + i];
            for (int i = 0; i < 5; ++i) st[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
        }
        // Iota
        st[0] ^= kRoundConstants[round];
    }
}

constexpr size_t kRate = 136;  // Bytes absorbed per permutation for a 256-bit output.

void absorb(uint64_t st[25], const uint8_t* block) {
    for (size_t i = 0; i < kRate / 8; ++i) {
        uint64_t lane = 0;
        for (int b = 7; b >= 0; --b) lane = (lane << 8) | block[i * 8 + b];  // Little-endian.
        st[i] ^= lane;
    }
    keccak_f(st);
}

}  // namespace

Hash keccak256(std::string_view data) {
    uint64_t st[25] = {};
    const auto* p = reinterpret_cast<const uint8_t*>(data.data());
    size_t left = data.size();
    while (left >= kRate) {
        absorb(st, p);
        p += kRate;
        left -= kRate;
    }
    // Last block with Keccak padding: 0x01 ... 0x80 (SHA3 would use 0x06).
    uint8_t last[kRate] = {};
    std::memcpy(last, p, left);
    last[left] ^= 0x01;
    last[kRate - 1] ^= 0x80;
    absorb(st, last);

    Hash out;
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(st[i / 8] >> (8 * (i % 8)));
    return out;
}

// ---------------------------------------------------------------------------
// Hex helpers
// ---------------------------------------------------------------------------

std::string to_hex(const uint8_t* data, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out = "0x";
    for (size_t i = 0; i < size; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 0x0f];
    }
    return out;
}

namespace {

// "0x1234..." -> bytes. Throws if it is not exactly `size` bytes of hex.
std::vector<uint8_t> from_hex(std::string_view text, size_t size, const char* what) {
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
    if (text.size() != size * 2) throw std::invalid_argument(std::string(what) + ": wrong length");
    auto digit = [what](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        throw std::invalid_argument(std::string(what) + ": not hexadecimal");
    };
    std::vector<uint8_t> out(size);
    for (size_t i = 0; i < size; ++i) out[i] = static_cast<uint8_t>(digit(text[2 * i]) << 4 | digit(text[2 * i + 1]));
    return out;
}

std::string_view as_view(const Hash& h) { return {reinterpret_cast<const char*>(h.data()), h.size()}; }

}  // namespace

// ---------------------------------------------------------------------------
// Action hash and EIP-712 digest
// ---------------------------------------------------------------------------

Hash action_hash(std::string_view action_msgpack, int64_t nonce, std::string_view vault_address) {
    std::string data(action_msgpack);
    for (int i = 7; i >= 0; --i) data += static_cast<char>((static_cast<uint64_t>(nonce) >> (8 * i)) & 0xff);
    if (vault_address.empty()) {
        data += '\0';
    } else {
        data += '\x01';
        for (uint8_t b : from_hex(vault_address, 20, "vault address")) data += static_cast<char>(b);
    }
    return keccak256(data);
}

namespace {

// The message Hyperliquid wants signed for an L1 action, as EIP-712:
//   keccak256(0x19 0x01 || domainSeparator || hashStruct(Agent))
// with domain {name "Exchange", version "1", chainId 1337, verifyingContract 0x0}
// and Agent {source: "a" | "b", connectionId: action hash}.
Hash eip712_agent_digest(const Hash& connection_id, bool mainnet) {
    // uint256(1337) as 32 big-endian bytes.
    std::string chain_id(32, '\0');
    chain_id[30] = static_cast<char>(1337 >> 8);
    chain_id[31] = static_cast<char>(1337 & 0xff);
    const std::string zero_address(32, '\0');  // An address is padded to 32 bytes.

    const Hash domain_type =
        keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)");
    const Hash domain_separator = keccak256(std::string(as_view(domain_type)) + std::string(as_view(keccak256("Exchange"))) +
                                            std::string(as_view(keccak256("1"))) + chain_id + zero_address);

    const Hash agent_type = keccak256("Agent(string source,bytes32 connectionId)");
    const Hash agent = keccak256(std::string(as_view(agent_type)) + std::string(as_view(keccak256(mainnet ? "a" : "b"))) +
                                 std::string(as_view(connection_id)));

    return keccak256(std::string("\x19\x01", 2) + std::string(as_view(domain_separator)) + std::string(as_view(agent)));
}

}  // namespace

// ---------------------------------------------------------------------------
// secp256k1 ECDSA with recovery id (OpenSSL BIGNUM / EC_POINT)
// ---------------------------------------------------------------------------

struct Signer::Key {
    EC_GROUP* group = nullptr;
    BIGNUM* order = nullptr;
    BIGNUM* priv = nullptr;
    BN_CTX* ctx = nullptr;

    ~Key() {
        BN_clear_free(priv);  // Clears the key from memory before freeing it.
        BN_free(order);
        EC_GROUP_free(group);
        BN_CTX_free(ctx);
    }
};

namespace {

// BIGNUM -> exactly 32 big-endian bytes.
std::array<uint8_t, 32> to_bytes32(const BIGNUM* bn) {
    std::array<uint8_t, 32> out{};
    if (BN_bn2binpad(bn, out.data(), 32) != 32) throw std::runtime_error("hyperliquid signer: number too large");
    return out;
}

void hmac_sha256(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data, std::vector<uint8_t>& out) {
    unsigned int len = 0;
    out.resize(32);
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), data.data(), data.size(), out.data(), &len) ||
        len != 32) {
        throw std::runtime_error("hyperliquid signer: HMAC failed");
    }
}

// RFC 6979: the ECDSA nonce k derived from the key and the hash, so the same
// input always gives the same signature (as eth_account does).
BIGNUM* deterministic_k(const BIGNUM* priv, const Hash& hash, const BIGNUM* order) {
    const auto x = to_bytes32(priv);
    std::vector<uint8_t> V(32, 0x01);
    std::vector<uint8_t> K(32, 0x00);

    auto mix = [&](uint8_t separator) {
        std::vector<uint8_t> data(V);
        data.push_back(separator);
        data.insert(data.end(), x.begin(), x.end());
        data.insert(data.end(), hash.begin(), hash.end());
        hmac_sha256(K, data, K);
        hmac_sha256(K, V, V);
    };
    mix(0x00);
    mix(0x01);

    BIGNUM* k = BN_new();
    while (true) {
        hmac_sha256(K, V, V);
        BN_bin2bn(V.data(), static_cast<int>(V.size()), k);
        if (!BN_is_zero(k) && BN_cmp(k, order) < 0) return k;
        std::vector<uint8_t> data(V);
        data.push_back(0x00);
        hmac_sha256(K, data, K);
        hmac_sha256(K, V, V);
    }
}

}  // namespace

Signer::Signer(std::string_view private_key_hex, bool mainnet) : mainnet_(mainnet), key_(std::make_unique<Key>()) {
    const auto key_bytes = from_hex(private_key_hex, 32, "private key");

    Key& k = *key_;
    k.ctx = BN_CTX_new();
    k.group = EC_GROUP_new_by_curve_name(NID_secp256k1);
    k.order = BN_new();
    k.priv = BN_bin2bn(key_bytes.data(), 32, nullptr);
    if (!k.ctx || !k.group || !k.order || !k.priv || !EC_GROUP_get_order(k.group, k.order, k.ctx)) {
        throw std::runtime_error("hyperliquid signer: OpenSSL initialization failed");
    }
    if (BN_is_zero(k.priv) || BN_cmp(k.priv, k.order) >= 0) {
        throw std::invalid_argument("private key: out of range");
    }

    // Address = last 20 bytes of keccak256(public key x || y).
    EC_POINT* pub = EC_POINT_new(k.group);
    uint8_t pub_bytes[65];
    const bool ok = pub && EC_POINT_mul(k.group, pub, k.priv, nullptr, nullptr, k.ctx) &&
                    EC_POINT_point2oct(k.group, pub, POINT_CONVERSION_UNCOMPRESSED, pub_bytes, 65, k.ctx) == 65;
    EC_POINT_free(pub);
    if (!ok) throw std::runtime_error("hyperliquid signer: public key derivation failed");
    const Hash h = keccak256({reinterpret_cast<const char*>(pub_bytes + 1), 64});  // Skip the 0x04 prefix.
    address_ = to_hex(h.data() + 12, 20);
}

Signer::~Signer() = default;

Signature Signer::sign_l1_action(std::string_view action_msgpack, int64_t nonce,
                                 std::string_view vault_address) const {
    const Hash digest = eip712_agent_digest(action_hash(action_msgpack, nonce, vault_address), mainnet_);
    const Key& key = *key_;

    BIGNUM* k = deterministic_k(key.priv, digest, key.order);
    EC_POINT* R = EC_POINT_new(key.group);
    BIGNUM* x = BN_new();
    BIGNUM* y = BN_new();
    BIGNUM* r = BN_new();
    BIGNUM* s = BN_new();
    BIGNUM* k_inv = BN_new();
    BIGNUM* e = BN_bin2bn(digest.data(), static_cast<int>(digest.size()), nullptr);
    BIGNUM* tmp = BN_new();
    BIGNUM* half_order = BN_new();

    // R = k * G; r = R.x mod n; s = k^-1 * (e + r * priv) mod n
    bool ok = R && x && y && r && s && k_inv && e && tmp && half_order &&
              EC_POINT_mul(key.group, R, k, nullptr, nullptr, key.ctx) &&
              EC_POINT_get_affine_coordinates(key.group, R, x, y, key.ctx) && BN_nnmod(r, x, key.order, key.ctx) &&
              BN_mod_inverse(k_inv, k, key.order, key.ctx) && BN_mod_mul(tmp, r, key.priv, key.order, key.ctx) &&
              BN_mod_add(tmp, e, tmp, key.order, key.ctx) && BN_mod_mul(s, k_inv, tmp, key.order, key.ctx) &&
              BN_rshift1(half_order, key.order);

    Signature sig;
    if (ok) {
        // Recovery id: lets the exchange find our address from the signature.
        // bit 0 = R.y is odd, bit 1 = R.x was >= n (practically never).
        int recovery_id = (BN_cmp(x, key.order) >= 0 ? 2 : 0) | (BN_is_odd(y) ? 1 : 0);
        // Ethereum accepts only the "low" s; using n - s mirrors R, so flip bit 0.
        if (BN_cmp(s, half_order) > 0) {
            ok = BN_sub(s, key.order, s);
            recovery_id ^= 1;
        }
        const auto r_bytes = to_bytes32(r);
        const auto s_bytes = to_bytes32(s);
        sig.r = to_hex(r_bytes.data(), 32);
        sig.s = to_hex(s_bytes.data(), 32);
        sig.v = 27 + recovery_id;
    }

    BN_clear_free(k);
    EC_POINT_free(R);
    BN_free(x);
    BN_free(y);
    BN_free(r);
    BN_free(s);
    BN_free(k_inv);
    BN_free(e);
    BN_free(tmp);
    BN_free(half_order);
    if (!ok) throw std::runtime_error("hyperliquid signer: signing failed");
    return sig;
}

}  // namespace peewee::hyperliquid
