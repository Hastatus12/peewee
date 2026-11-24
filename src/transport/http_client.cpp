#include "transport/http_client.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <chrono>

#include "transport/url.hpp"

namespace peewee::transport {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace {

constexpr auto kTimeout = std::chrono::seconds(10);

// Parameters by value: the coroutine outlives the caller's arguments' scope.
asio::awaitable<HttpResponse> run_request(ssl::context& ctx, Url url, bool is_post, std::string body,
                                          HttpHeaders headers) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::resolver resolver(executor);
    beast::ssl_stream<beast::tcp_stream> stream(executor, ctx);

    if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str())) {
        throw beast::system_error(beast::error_code(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()));
    }
    stream.set_verify_callback(ssl::host_name_verification(url.host));

    const auto results = co_await resolver.async_resolve(url.host, url.port, asio::use_awaitable);
    beast::get_lowest_layer(stream).expires_after(kTimeout);
    co_await beast::get_lowest_layer(stream).async_connect(results, asio::use_awaitable);
    beast::get_lowest_layer(stream).expires_after(kTimeout);
    co_await stream.async_handshake(ssl::stream_base::client, asio::use_awaitable);

    http::request<http::string_body> req{is_post ? http::verb::post : http::verb::get, url.target, 11};
    req.set(http::field::host, url.host);
    req.set(http::field::user_agent, "peewee");
    for (const auto& [name, value] : headers) {
        req.set(name, value);
    }
    if (is_post) {
        req.set(http::field::content_type, "application/json");
        req.body() = std::move(body);
    }
    req.prepare_payload();

    beast::get_lowest_layer(stream).expires_after(kTimeout);
    co_await http::async_write(stream, req, asio::use_awaitable);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    co_await http::async_read(stream, buffer, res, asio::use_awaitable);

    // Polite TLS close; many servers just drop the connection, so errors are ignored.
    beast::error_code ignored;
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(2));
    co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, ignored));

    co_return HttpResponse{static_cast<int>(res.result_int()), std::move(res.body())};
}

}  // namespace

HttpClient::HttpClient(std::string base_url) : base_url_(std::move(base_url)) {}

HttpClient::~HttpClient() = default;

HttpResponse HttpClient::get(std::string_view path_and_query, const HttpHeaders& headers) {
    return request(false, path_and_query, {}, headers);
}

HttpResponse HttpClient::post(std::string_view path, std::string_view body, const HttpHeaders& headers) {
    return request(true, path, body, headers);
}

HttpResponse HttpClient::request(bool is_post, std::string_view target, std::string_view body,
                                 const HttpHeaders& headers) {
    Url url = parse_url(base_url_ + std::string(target));

    ssl::context ctx{ssl::context::tls_client};
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(ssl::verify_peer);

    asio::io_context io;
    auto result = asio::co_spawn(io, run_request(ctx, std::move(url), is_post, std::string(body), headers),
                                 asio::use_future);
    io.run();
    return result.get();  // Rethrows network errors.
}

}  // namespace peewee::transport
