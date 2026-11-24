#pragma once
// Splits "wss://host[:port]/path" or "https://host[:port]/path" into parts.

#include <stdexcept>
#include <string>
#include <string_view>

namespace peewee::transport {

struct Url {
    std::string host;
    std::string port;
    std::string target;  // Path and query, at least "/".
};

inline Url parse_url(std::string_view url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        throw std::invalid_argument("invalid url: " + std::string(url));
    }
    const std::string_view scheme = url.substr(0, scheme_end);
    if (scheme != "wss" && scheme != "https") {
        throw std::invalid_argument("only wss:// and https:// are supported: " + std::string(url));
    }

    std::string_view rest = url.substr(scheme_end + 3);
    const auto path_start = rest.find('/');
    const std::string_view authority = rest.substr(0, path_start);

    Url result;
    result.target = path_start == std::string_view::npos ? "/" : std::string(rest.substr(path_start));
    const auto colon = authority.find(':');
    result.host = std::string(authority.substr(0, colon));
    result.port = colon == std::string_view::npos ? "443" : std::string(authority.substr(colon + 1));
    return result;
}

}  // namespace peewee::transport
