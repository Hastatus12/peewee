#pragma once
// Generic WebSocket client (TLS) with a persistent connection.
// Knows nothing about exchanges: transports text only.
//
// PRIVATE to the library: used only by adapters in src/venues.
//
// Threading: everything runs on the io_context passed to the constructor,
// OWNED BY THE ADAPTER and shared by all of its connections (one network
// thread per venue). Handlers are always called on that thread.
// connect(), send() and close() may be called from any thread.
// Destroy the client only after that io_context has stopped.

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace boost::asio {
class io_context;
}

namespace peewee::transport {

class WsClient {
public:
    struct Handlers {
        std::function<void()> on_open;
        // The view is valid only during the call (it points into the read buffer).
        std::function<void(std::string_view message)> on_message;
        // Connection lost (not called after close()). The adapter reconnects.
        std::function<void(std::string_view reason)> on_close;
    };

    // url: "wss://host[:port]/path"
    // idle_timeout: nothing received for this long = dead connection (closed,
    // on_close called). The adapter must make the exchange send something
    // more often than that (pings), or a quiet connection is cut.
    WsClient(boost::asio::io_context& io, std::string url, Handlers handlers,
             std::chrono::seconds idle_timeout = std::chrono::seconds(60));
    ~WsClient();

    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    void connect();               // Asynchronous: on_open is called when ready. Also reconnects.
    void send(std::string text);  // Dropped if the connection is not open.
    void close();
    bool is_open() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace peewee::transport
