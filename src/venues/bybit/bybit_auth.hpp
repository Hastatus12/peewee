#pragma once
// Bybit request signing.

#include <cstdint>
#include <string>
#include <string_view>

namespace peewee::bybit {

// HMAC-SHA256(key, message) as lowercase hexadecimal.
std::string hmac_sha256_hex(std::string_view key, std::string_view message);

// Signature for private WebSocket authentication:
// HMAC-SHA256(secret, "GET/realtime" + expires_ms)
std::string ws_auth_signature(std::string_view api_secret, int64_t expires_ms);

// Signature for REST v5:
// HMAC-SHA256(secret, timestamp + api_key + recv_window + (query_string | body))
std::string rest_signature(std::string_view api_secret, std::string_view api_key, int64_t timestamp_ms,
                           int recv_window_ms, std::string_view payload);

}  // namespace peewee::bybit
