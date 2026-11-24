#pragma once
// Generic HTTPS client for REST APIs.
//
// PRIVATE to the library: used only by adapters in src/venues.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace peewee::transport {

struct HttpResponse {
    int status = 0;
    std::string body;
};

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

class HttpClient {
public:
    explicit HttpClient(std::string base_url);  // E.g. "https://api.bybit.com".
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Blocking (10 s timeout per step), throws on network errors: use ONLY
    // outside the critical path (startup, reconciliation, occasional
    // queries), never on the engine or network thread.
    HttpResponse get(std::string_view path_and_query, const HttpHeaders& headers = {});
    HttpResponse post(std::string_view path, std::string_view body, const HttpHeaders& headers = {});

private:
    HttpResponse request(bool is_post, std::string_view target, std::string_view body, const HttpHeaders& headers);

    std::string base_url_;
    // TODO: reusable keep-alive connection (TLS handshakes cost tens of ms).
    // Today every request opens a new connection.
};

}  // namespace peewee::transport
