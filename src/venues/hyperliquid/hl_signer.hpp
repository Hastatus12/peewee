#pragma once
// Hyperliquid action signing.
//
// Unlike Bybit (HMAC with a secret key), Hyperliquid uses
// Ethereum-style cryptographic signatures (EIP-712) with a private key:
//   1. Serialize the action as msgpack (see hl_msgpack.hpp).
//   2. Append nonce (8-byte big-endian) and the vault flag/address.
//   3. Apply keccak256 to everything -> "connectionId".
//   4. Build the "phantom agent" {source: "a" mainnet | "b" testnet, connectionId}.
//   5. Sign EIP-712 with secp256k1 -> (r, s, v).
//
// Verified byte for byte against the official Python SDK
// (hyperliquid-python-sdk, sign_l1_action) in tests/test_hl_signer.cpp.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace peewee::hyperliquid {

using Hash = std::array<uint8_t, 32>;

struct Signature {
    std::string r;  // 0x + 64 hex digits
    std::string s;  // 0x + 64 hex digits
    int v = 27;     // 27 or 28
};

// Ethereum's Keccak-256 (NOT the same as SHA3-256: different padding).
Hash keccak256(std::string_view data);

// Step 1-3: keccak256(msgpack || nonce || vault). vault_address is
// "0x" + 40 hex digits, or empty for none.
Hash action_hash(std::string_view action_msgpack, int64_t nonce, std::string_view vault_address = {});

// "0x" + lowercase hex.
std::string to_hex(const uint8_t* data, size_t size);

class Signer {
public:
    // private_key_hex: "0x" + 64 hex digits (the API wallet's key).
    // Throws std::invalid_argument if it is not a valid key.
    Signer(std::string_view private_key_hex, bool mainnet);
    ~Signer();

    Signer(const Signer&) = delete;
    Signer& operator=(const Signer&) = delete;

    // The address of this key ("0x..." lowercase): compare it with the API
    // wallet address shown by Hyperliquid to check the key is the right one.
    const std::string& address() const { return address_; }

    // action_msgpack: action already serialized as msgpack.
    Signature sign_l1_action(std::string_view action_msgpack, int64_t nonce,
                             std::string_view vault_address = {}) const;

private:
    struct Key;  // OpenSSL objects, hidden in hl_signer.cpp.

    bool mainnet_;
    std::unique_ptr<Key> key_;
    std::string address_;
};

}  // namespace peewee::hyperliquid
