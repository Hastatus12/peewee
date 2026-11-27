#pragma once
// Lighter transaction signing: create / modify / cancel order and the auth
// token of private streams.
//
// Each transaction is hashed field by field (Poseidon2, see lighter_crypto.hpp)
// and signed with the API key. The result is the "tx_info" JSON that goes,
// unchanged, into a "jsonapi/sendtx" message (see lighter_codec.hpp).
//
// Same output as lighter-go (types/txtypes/*.go, client/tx_client.go), checked
// byte for byte in tests/test_lighter_signer.cpp. No transaction attributes
// (integrator fees, skip-nonce, ...) are used: they would change the hash.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "lighter_crypto.hpp"

namespace peewee::lighter {

// Transaction types (lighter-go txtypes/constants.go).
inline constexpr uint8_t kTxCreateOrder = 14;
inline constexpr uint8_t kTxCancelOrder = 15;
inline constexpr uint8_t kTxModifyOrder = 17;

// L2 chain IDs: part of every signed hash, so a mainnet signature is refused on testnet.
inline constexpr uint32_t kMainnetChainId = 304;
inline constexpr uint32_t kTestnetChainId = 300;

// Fields every transaction carries besides its own.
struct TxHeader {
    int64_t expired_at = 0;  // Unix ms: the transaction is refused after this.
    int64_t nonce = 0;       // Per API key, +1 for every transaction.
};

// Values as Lighter's API takes them: integers in the market's decimals.
struct CreateOrderTx {
    int64_t market_index = 0;
    int64_t client_order_index = 0;  // At most 2^48 - 1.
    int64_t base_amount = 0;         // size * 10^size_decimals.
    uint32_t price = 0;              // price * 10^price_decimals; worst price for market types.
    uint8_t is_ask = 0;
    uint8_t type = 0;                // 0 limit, 1 market, 2 stop-loss, 3 stop-loss limit,
                                     // 4 take-profit, 5 take-profit limit.
    uint8_t time_in_force = 0;       // 0 IOC, 1 good-till-time, 2 post-only.
    uint8_t reduce_only = 0;
    uint32_t trigger_price = 0;
    int64_t order_expiry = 0;        // Unix ms; 0 for IOC.
};

struct ModifyOrderTx {
    int64_t market_index = 0;
    int64_t index = 0;  // Client order index (or Lighter's order index).
    int64_t base_amount = 0;
    uint32_t price = 0;
    uint32_t trigger_price = 0;
};

struct CancelOrderTx {
    int64_t market_index = 0;
    int64_t index = 0;  // Client order index (or Lighter's order index).
};

class Signer {
public:
    // Where the secret Schnorr nonce k comes from: random_scalar() by default.
    // Tests pass a fixed k to reproduce lighter-go's signatures exactly.
    using NonceSource = std::function<Scalar()>;

    // private_key_hex: the API key's private key, 40 bytes in hex ("0x" optional),
    // as lighter-go prints it. Throws std::invalid_argument if malformed.
    Signer(std::string_view private_key_hex, uint32_t chain_id, int64_t account_index, uint8_t api_key_index,
           NonceSource nonce_source = random_scalar);

    // The public key, 40 bytes as lowercase hex without "0x": the form
    // Lighter shows in GET /api/v1/apikeys, to check the key is the right one.
    std::string public_key_hex() const;

    int64_t account_index() const { return account_index_; }
    uint8_t api_key_index() const { return api_key_index_; }

    // Signed "tx_info" JSON, e.g. for create:
    // {"AccountIndex":1,"ApiKeyIndex":3,"MarketIndex":1,"ClientOrderIndex":42,"BaseAmount":2000,
    //  "Price":6500012,"IsAsk":0,"Type":0,"TimeInForce":2,"ReduceOnly":0,"TriggerPrice":0,
    //  "OrderExpiry":1800000000000,"ExpiredAt":1700000600000,"Nonce":7,"Sig":"<base64>","L2TxAttributes":null}
    std::string sign_create_order(const CreateOrderTx& tx, const TxHeader& h) const;
    std::string sign_modify_order(const ModifyOrderTx& tx, const TxHeader& h) const;
    std::string sign_cancel_order(const CancelOrderTx& tx, const TxHeader& h) const;

    // Auth token for private WebSocket channels and authenticated REST calls:
    // "<deadline>:<account>:<api key>:<signature hex>". deadline: Unix
    // seconds, at most 8 hours ahead.
    std::string auth_token(int64_t deadline_s) const;

    // The hashes that get signed, exposed for tests.
    Fp5 hash_create_order(const CreateOrderTx& tx, const TxHeader& h) const;
    Fp5 hash_modify_order(const ModifyOrderTx& tx, const TxHeader& h) const;
    Fp5 hash_cancel_order(const CancelOrderTx& tx, const TxHeader& h) const;

private:
    Signature sign(const Fp5& hash) const;

    Scalar key_;
    uint32_t chain_id_;
    int64_t account_index_;
    uint8_t api_key_index_;
    NonceSource nonce_source_;
};

}  // namespace peewee::lighter
