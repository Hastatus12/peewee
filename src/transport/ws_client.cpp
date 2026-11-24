#include "transport/ws_client.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <chrono>
#include <deque>
#include <stdexcept>

#include "transport/url.hpp"

namespace peewee::transport {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = boost::asio::ssl;
namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;

// Every async operation captures the generation it belongs to. A new
// connection or a close bumps it, so callbacks from an old stream (which
// still arrive, with operation_aborted) are ignored.
struct WsClient::Impl {
    using Stream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    Impl(asio::io_context& io_, std::string url, Handlers handlers_, std::chrono::seconds idle_timeout_)
        : io(io_), endpoint(parse_url(url)), handlers(std::move(handlers_)), idle_timeout(idle_timeout_),
          resolver(io_) {
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(ssl::verify_peer);
    }

    void start() {
        const uint64_t g = ++gen;
        open = false;
        outbox.clear();
        writing = false;
        buffer.clear();
        ws = std::make_unique<Stream>(io, ssl_ctx);

        // DNS -> TCP -> TLS -> WebSocket handshake, one step per function.
        resolver.async_resolve(endpoint.host, endpoint.port,
                               [this, g](beast::error_code ec, tcp::resolver::results_type results) {
                                   if (g == gen) on_resolved(g, ec, results);
                               });
    }

    void on_resolved(uint64_t g, beast::error_code ec, const tcp::resolver::results_type& results) {
        if (ec) return fail("resolve", ec);
        beast::get_lowest_layer(*ws).expires_after(std::chrono::seconds(30));
        beast::get_lowest_layer(*ws).async_connect(results, [this, g](beast::error_code e, const tcp::endpoint&) {
            if (g == gen) on_connected(g, e);
        });
    }

    void on_connected(uint64_t g, beast::error_code ec) {
        if (ec) return fail("connect", ec);
        beast::get_lowest_layer(*ws).socket().set_option(tcp::no_delay(true), ec);
        // SNI: without it the TLS server does not know which certificate to send.
        if (!SSL_set_tlsext_host_name(ws->next_layer().native_handle(), endpoint.host.c_str())) {
            return fail("sni", beast::error_code(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()));
        }
        ws->next_layer().set_verify_callback(ssl::host_name_verification(endpoint.host));
        ws->next_layer().async_handshake(ssl::stream_base::client, [this, g](beast::error_code e) {
            if (g == gen) on_tls_ready(g, e);
        });
    }

    void on_tls_ready(uint64_t g, beast::error_code ec) {
        if (ec) return fail("tls handshake", ec);
        beast::get_lowest_layer(*ws).expires_never();
        websocket::stream_base::timeout timeouts{};
        timeouts.handshake_timeout = std::chrono::seconds(30);
        // No data for idle_timeout = dead connection. The adapter pings more
        // often than that, and the exchange's pongs count as data.
        timeouts.idle_timeout = idle_timeout;
        timeouts.keep_alive_pings = false;
        ws->set_option(timeouts);
        ws->async_handshake(endpoint.host, endpoint.target, [this, g](beast::error_code e) {
            if (g == gen) on_ws_ready(g, e);
        });
    }

    void on_ws_ready(uint64_t g, beast::error_code ec) {
        if (ec) return fail("ws handshake", ec);
        ws->text(true);
        open = true;
        if (handlers.on_open) handlers.on_open();
        if (g == gen) read(g);  // on_open may have closed the connection.
    }

    void read(uint64_t g) {
        ws->async_read(buffer, [this, g](beast::error_code ec, std::size_t) {
            if (g != gen) return;
            if (ec) return fail("read", ec);
            const auto data = buffer.cdata();
            if (handlers.on_message) {
                handlers.on_message(std::string_view(static_cast<const char*>(data.data()), data.size()));
            }
            buffer.consume(buffer.size());
            if (g == gen) read(g);  // The handler may have closed the connection.
        });
    }

    void send(std::string text) {
        if (!open) return;
        outbox.push_back(std::move(text));
        if (!writing) write_next(gen);
    }

    // Beast allows one write at a time: queue the others.
    void write_next(uint64_t g) {
        writing = true;
        ws->async_write(asio::buffer(outbox.front()), [this, g](beast::error_code ec, std::size_t) {
            if (g != gen) return;
            if (ec) return fail("write", ec);
            outbox.pop_front();
            if (outbox.empty()) {
                writing = false;
            } else {
                write_next(g);
            }
        });
    }

    void shut() {
        ++gen;
        open = false;
        if (ws) {
            beast::error_code ignored;
            beast::get_lowest_layer(*ws).socket().close(ignored);
        }
    }

    void fail(const char* what, beast::error_code ec) {
        shut();
        if (handlers.on_close) handlers.on_close(std::string(what) + ": " + ec.message());
    }

    asio::io_context& io;
    Url endpoint;
    Handlers handlers;
    std::chrono::seconds idle_timeout;
    ssl::context ssl_ctx{ssl::context::tls_client};
    tcp::resolver resolver;
    std::unique_ptr<Stream> ws;
    beast::flat_buffer buffer;
    std::deque<std::string> outbox;
    bool writing = false;
    uint64_t gen = 0;
    std::atomic<bool> open{false};
};

WsClient::WsClient(asio::io_context& io, std::string url, Handlers handlers, std::chrono::seconds idle_timeout)
    : impl_(std::make_unique<Impl>(io, std::move(url), std::move(handlers), idle_timeout)) {}

WsClient::~WsClient() = default;

void WsClient::connect() {
    asio::post(impl_->io, [impl = impl_.get()] { impl->start(); });
}

void WsClient::send(std::string text) {
    asio::post(impl_->io, [impl = impl_.get(), text = std::move(text)]() mutable { impl->send(std::move(text)); });
}

void WsClient::close() {
    asio::post(impl_->io, [impl = impl_.get()] { impl->shut(); });
}

bool WsClient::is_open() const {
    return impl_->open;
}

}  // namespace peewee::transport
