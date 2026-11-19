#ifndef NAKAMA_WEBSOCKET_TRANSPORT_HPP
#define NAKAMA_WEBSOCKET_TRANSPORT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// En la siguiente version vamos a hacer el stop de forma segura para tarminar con seguridad
// osea cierre seguro esperar a que termine y tampoco esperar demasiado para liberar recursos del sistema
namespace nakama {

// Shared libcurl initialization for HTTP authentication and WebSocket.
bool initialize_websocket_library();


    enum class TransportState {
        STOPPED,
        CONNECTING,
        CONNECTED,
        CLOSING,
        ERROR
    };

    enum class WebSocketMessageType {
        TEXT,
        BINARY
    };

    struct WebSocketMessage {
        WebSocketMessageType type = WebSocketMessageType::TEXT;
        std::string payload;
    };

    struct WebSocketConfig {
        std::string url;

        long connect_timeout_ms = 10000;
        long io_poll_interval_ms = 100;

        std::size_t max_message_bytes = 1024 * 1024;
        std::size_t outgoing_queue_capacity = 256;
        std::size_t incoming_queue_capacity = 256;

        bool verify_tls = true;
    };

    class WebSocketTransport {
    public:
        explicit WebSocketTransport(WebSocketConfig config);

        ~WebSocketTransport();

        WebSocketTransport(const WebSocketTransport&) = delete;
        WebSocketTransport& operator=(const WebSocketTransport&) = delete;

        WebSocketTransport(WebSocketTransport&&) = delete;
        WebSocketTransport& operator=(WebSocketTransport&&) = delete;

        bool start();

        void stop();

        bool send_text(const std::string& message);

        bool send_binary(const std::string& message);

        bool try_receive(WebSocketMessage& message);

        bool try_receive(std::string& message);

        TransportState state() const;

        bool is_connected() const;

        std::string last_error() const;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace nakama

#endif
