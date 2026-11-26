#include "lighter_signer.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace peewee::lighter {

namespace {

// Signed values as field elements. Every value here is >= 0 and < p.
uint64_t fe(int64_t v) { return static_cast<uint64_t>(v); }

std::string base64(const Signature& sig) {
    std::array<unsigned char, 4 * ((sizeof(Signature) + 2) / 3) + 1> out{};
    const int n = EVP_EncodeBlock(out.data(), sig.data(), static_cast<int>(sig.size()));
    return std::string(reinterpret_cast<const char*>(out.data()), static_cast<size_t>(n));
}

std::string hex(const uint8_t* data, size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out += kDigits[data[i] >> 4];
        out += kDigits[data[i] & 0xf];
    }
    return out;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Scalar parse_private_key(std::string_view text) {
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
    if (text.size() != 80) {
        throw std::invalid_argument("lighter: the API private key must be 40 bytes (80 hex digits)");
    }
    std::array<uint8_t, 40> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        const int hi = hex_value(text[2 * i]);
        const int lo = hex_value(text[2 * i + 1]);
        if (hi < 0 || lo < 0) throw std::invalid_argument("lighter: the API private key is not hex");
        bytes[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return scalar_from_bytes(bytes);
}

}  // namespace

Signer::Signer(std::string_view private_key_hex, uint32_t chain_id, int64_t account_index, uint8_t api_key_index,
               NonceSource nonce_source)
    : key_(parse_private_key(private_key_hex)),
      chain_id_(chain_id),
      account_index_(account_index),
      api_key_index_(api_key_index),
      nonce_source_(std::move(nonce_source)) {}

std::string Signer::public_key_hex() const {
    const Bytes40 pk = to_bytes(public_key(key_));
    return hex(pk.data(), pk.size());
}

Signature Signer::sign(const Fp5& hash) const { return schnorr_sign(hash, key_, nonce_source_()); }

// Field order of each hash: lighter-go types/txtypes/<tx>.go, Hash().

Fp5 Signer::hash_create_order(const CreateOrderTx& tx, const TxHeader& h) const {
    const std::array<uint64_t, 16> e = {
        chain_id_,         kTxCreateOrder,        fe(h.nonce),     fe(h.expired_at),
        fe(account_index_), api_key_index_,       fe(tx.market_index), fe(tx.client_order_index),
        fe(tx.base_amount), tx.price,             tx.is_ask,       tx.type,
        tx.time_in_force,  tx.reduce_only,        tx.trigger_price, fe(tx.order_expiry)};
    return hash_to_fp5(e);
}

Fp5 Signer::hash_modify_order(const ModifyOrderTx& tx, const TxHeader& h) const {
    const std::array<uint64_t, 11> e = {
        chain_id_,      kTxModifyOrder,     fe(h.nonce),    fe(h.expired_at), fe(account_index_), api_key_index_,
        fe(tx.market_index), fe(tx.index), fe(tx.base_amount), tx.price,     tx.trigger_price};
    return hash_to_fp5(e);
}

Fp5 Signer::hash_cancel_order(const CancelOrderTx& tx, const TxHeader& h) const {
    const std::array<uint64_t, 8> e = {chain_id_,          kTxCancelOrder, fe(h.nonce),         fe(h.expired_at),
                                       fe(account_index_), api_key_index_, fe(tx.market_index), fe(tx.index)};
    return hash_to_fp5(e);
}

// The JSON below must keep lighter-go's field order and names (Go's
// json.Marshal of the tx struct): Lighter re-hashes what it receives.

std::string Signer::sign_create_order(const CreateOrderTx& tx, const TxHeader& h) const {
    const std::string sig = base64(sign(hash_create_order(tx, h)));
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  R"({"AccountIndex":%lld,"ApiKeyIndex":%u,"MarketIndex":%lld,"ClientOrderIndex":%lld,)"
                  R"("BaseAmount":%lld,"Price":%u,"IsAsk":%u,"Type":%u,"TimeInForce":%u,"ReduceOnly":%u,)"
                  R"("TriggerPrice":%u,"OrderExpiry":%lld,"ExpiredAt":%lld,"Nonce":%lld,"Sig":"%s",)"
                  R"("L2TxAttributes":null})",
                  static_cast<long long>(account_index_), unsigned{api_key_index_},
                  static_cast<long long>(tx.market_index), static_cast<long long>(tx.client_order_index),
                  static_cast<long long>(tx.base_amount), unsigned{tx.price}, unsigned{tx.is_ask},
                  unsigned{tx.type}, unsigned{tx.time_in_force}, unsigned{tx.reduce_only},
                  unsigned{tx.trigger_price}, static_cast<long long>(tx.order_expiry),
                  static_cast<long long>(h.expired_at), static_cast<long long>(h.nonce), sig.c_str());
    return buf;
}

std::string Signer::sign_modify_order(const ModifyOrderTx& tx, const TxHeader& h) const {
    const std::string sig = base64(sign(hash_modify_order(tx, h)));
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  R"({"AccountIndex":%lld,"ApiKeyIndex":%u,"MarketIndex":%lld,"Index":%lld,"BaseAmount":%lld,)"
                  R"("Price":%u,"TriggerPrice":%u,"ExpiredAt":%lld,"Nonce":%lld,"Sig":"%s","L2TxAttributes":null})",
                  static_cast<long long>(account_index_), unsigned{api_key_index_},
                  static_cast<long long>(tx.market_index), static_cast<long long>(tx.index),
                  static_cast<long long>(tx.base_amount), unsigned{tx.price}, unsigned{tx.trigger_price},
                  static_cast<long long>(h.expired_at), static_cast<long long>(h.nonce), sig.c_str());
    return buf;
}

std::string Signer::sign_cancel_order(const CancelOrderTx& tx, const TxHeader& h) const {
    const std::string sig = base64(sign(hash_cancel_order(tx, h)));
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  R"({"AccountIndex":%lld,"ApiKeyIndex":%u,"MarketIndex":%lld,"Index":%lld,"ExpiredAt":%lld,)"
                  R"("Nonce":%lld,"Sig":"%s","L2TxAttributes":null})",
                  static_cast<long long>(account_index_), unsigned{api_key_index_},
                  static_cast<long long>(tx.market_index), static_cast<long long>(tx.index),
                  static_cast<long long>(h.expired_at), static_cast<long long>(h.nonce), sig.c_str());
    return buf;
}

std::string Signer::auth_token(int64_t deadline_s) const {
    // The message's bytes, 8 per field element (little-endian, zero-padded),
    // as lighter-go types.ConstructAuthToken. ASCII bytes keep each 8-byte
    // word < p, so no reduction happens.
    const std::string message = std::to_string(deadline_s) + ":" + std::to_string(account_index_) + ":" +
                                std::to_string(unsigned{api_key_index_});
    std::vector<uint64_t> elements((message.size() + 7) / 8, 0);
    for (size_t i = 0; i < message.size(); ++i) {
        elements[i / 8] |= static_cast<uint64_t>(static_cast<uint8_t>(message[i])) << (8 * (i % 8));
    }
    const Signature sig = sign(hash_to_fp5(elements));
    return message + ":" + hex(sig.data(), sig.size());
}

}  // namespace peewee::lighter
