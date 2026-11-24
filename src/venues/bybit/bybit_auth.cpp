#include "bybit_auth.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <stdexcept>

namespace peewee::bybit {

std::string hmac_sha256_hex(std::string_view key, std::string_view message) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest, &length)) {
        throw std::runtime_error("hmac_sha256_hex: OpenSSL HMAC failed");
    }

    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(length * 2);
    for (unsigned int i = 0; i < length; ++i) {
        result.push_back(hex[digest[i] >> 4]);
        result.push_back(hex[digest[i] & 0x0f]);
    }
    return result;
}

std::string ws_auth_signature(std::string_view api_secret, int64_t expires_ms) {
    return hmac_sha256_hex(api_secret, "GET/realtime" + std::to_string(expires_ms));
}

std::string rest_signature(std::string_view api_secret, std::string_view api_key, int64_t timestamp_ms,
                           int recv_window_ms, std::string_view payload) {
    std::string msg = std::to_string(timestamp_ms);
    msg += api_key;
    msg += std::to_string(recv_window_ms);
    msg += payload;
    return hmac_sha256_hex(api_secret, msg);
}

}  // namespace peewee::bybit
