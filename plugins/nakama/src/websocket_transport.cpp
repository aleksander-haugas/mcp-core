
#include "nakama/websocket_transport.hpp"

#include <curl/curl.h>
#include <curl/websockets.h>

#include <poll.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace nakama {
    namespace {

        constexpr std::size_t IO_BUFFER_SIZE = 16384;
        constexpr std::size_t MAX_QUEUE_BYTES = 16 * 1024 * 1024;

        CURLcode initialize_curl_global()
        {
            static std::once_flag init_flag;
            static CURLcode init_result = CURLE_FAILED_INIT;

            std::call_once(init_flag, [] {
                init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
            });

            return init_result;
        }

        bool valid_config(const WebSocketConfig& config)
        {
            const bool valid_url =
            config.url.compare(0, 5, "ws://") == 0 ||
            config.url.compare(0, 6, "wss://") == 0;

            return valid_url &&
            config.connect_timeout_ms > 0 &&
            config.io_poll_interval_ms > 0 &&
            config.io_poll_interval_ms <= INT_MAX &&
            config.connect_timeout_ms <= INT_MAX &&
            config.max_message_bytes > 0 &&
            config.outgoing_queue_capacity > 0 &&
            config.incoming_queue_capacity > 0;
        }

    } // namespace

    bool initialize_websocket_library()
    {
        return initialize_curl_global() == CURLE_OK;
    }

    class WebSocketTransport::Impl {
    public:
        explicit Impl(WebSocketConfig config)
        : config_(std::move(config))
        {
        }

        ~Impl()
        {
            stop();

            if (wake_read_fd_ >= 0) {
                close(wake_read_fd_);
                wake_read_fd_ = -1;
            }

            if (wake_write_fd_ >= 0) {
                close(wake_write_fd_);
                wake_write_fd_ = -1;
            }
        }

        bool start()
        {
            std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

            if (!valid_config(config_)) {
                return false;
            }

            if (!initialize_wake_pipe()) {
                return false;
            }

            if (worker_.joinable()) {
                {
                    std::lock_guard<std::mutex> state_lock(state_mutex_);

                    if (state_ == TransportState::CONNECTING ||
                        state_ == TransportState::CONNECTED) {
                        state_ = TransportState::CLOSING;
                        }
                }

                worker_.join();
            }
            if (initialize_curl_global() != CURLE_OK) {
                set_error("curl_global_init failed");
                return false;
            }

            {
                std::lock_guard<std::mutex> queue_lock(queue_mutex_);

                outgoing_.clear();
                incoming_.clear();

                outgoing_bytes_ = 0;
                incoming_bytes_ = 0;
            }

            receive_buffer_.clear();
            ping_buffer_.clear();
            receive_message_type_ = WebSocketMessageType::TEXT;
            receive_message_active_ = false;

            stop_requested_.store(false);

            {
                std::lock_guard<std::mutex> state_lock(state_mutex_);

                last_error_.clear();
                state_ = TransportState::CONNECTING;
            }

            try {
                worker_ = std::thread(&Impl::run, this);
            } catch (...) {
                set_error("Unable to create WebSocket worker thread");
                return false;
            }

            return true;
        }

        void stop()
        {
            std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

            stop_requested_.store(true);
            (void)signal_wake();
            wake_multi();

            {
                std::lock_guard<std::mutex> state_lock(state_mutex_);

                if (state_ == TransportState::CONNECTING ||
                    state_ == TransportState::CONNECTED) {
                    state_ = TransportState::CLOSING;
                }
            }

            if (worker_.joinable()) {
                worker_.join();
            }

            if (state() != TransportState::ERROR) {
                set_state(TransportState::STOPPED);
            }
        }

        bool send_message_to_queue(
            WebSocketMessageType type,
            const std::string& message)
        {
            if (message.size() > config_.max_message_bytes) {
                return false;
            }

            if (state() != TransportState::CONNECTED ||
                stop_requested_.load()) {
                return false;
            }

            std::lock_guard<std::mutex> lock(queue_mutex_);

            if (outgoing_.size() >= config_.outgoing_queue_capacity) {
                return false;
            }

            constexpr std::size_t MAX_QUEUE_BYTES =
                16 * 1024 * 1024;

            if (outgoing_bytes_ > MAX_QUEUE_BYTES ||
                message.size() > MAX_QUEUE_BYTES - outgoing_bytes_) {
                return false;
            }

            outgoing_.push_back(WebSocketMessage{type, message});
            outgoing_bytes_ += message.size();

            // The message remains queued if waking the worker fails.
            // The worker also polls periodically.
            (void)signal_wake();

            return true;
        }

        bool send_text(const std::string& message)
        {
            return send_message_to_queue(
                WebSocketMessageType::TEXT, message);
        }

        bool send_binary(const std::string& message)
        {
            return send_message_to_queue(
                WebSocketMessageType::BINARY, message);
        }

        bool try_receive(WebSocketMessage& message)
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);

            if (incoming_.empty()) {
                return false;
            }

            message = std::move(incoming_.front());
            incoming_bytes_ -= message.payload.size();
            incoming_.pop_front();

            return true;
        }

        bool try_receive(std::string& message)
        {
            WebSocketMessage received;

            if (!try_receive(received)) {
                return false;
            }

            message = std::move(received.payload);
            return true;
        }

        TransportState state() const
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            return state_;
        }

        bool is_connected() const
        {
            return state() == TransportState::CONNECTED;
        }

        std::string last_error() const
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            return last_error_;
        }

    private:
        bool initialize_wake_pipe()
        {
            if (wake_read_fd_ >= 0 && wake_write_fd_ >= 0) {
                return true;
            }

            int descriptors[2] = {-1, -1};

            if (pipe2(descriptors, O_NONBLOCK | O_CLOEXEC) != 0) {
                set_error("Unable to create WebSocket wake pipe");
                return false;
            }

            wake_read_fd_ = descriptors[0];
            wake_write_fd_ = descriptors[1];

            return true;
        }

        bool signal_wake()
        {
            if (wake_write_fd_ < 0) {
                return false;
            }

            const char byte = 1;

            for (;;) {
                const ssize_t result = write(
                    wake_write_fd_,
                    &byte,
                    sizeof(byte)
                );

                if (result == static_cast<ssize_t>(sizeof(byte))) {
                    return true;
                }

                if (result < 0 && errno == EINTR) {
                    continue;
                }

                if (result < 0 &&
                    (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    return true;
                    }

                    return false;
            }
        }

        void publish_multi(CURLM* multi)
        {
            std::lock_guard<std::mutex> lock(multi_mutex_);
            active_multi_ = multi;
        }

        void clear_multi(CURLM* multi)
        {
            std::lock_guard<std::mutex> lock(multi_mutex_);

            if (active_multi_ == multi) {
                active_multi_ = nullptr;
            }
        }

        void wake_multi()
        {
            std::lock_guard<std::mutex> lock(multi_mutex_);

            if (active_multi_ != nullptr) {
                (void)curl_multi_wakeup(active_multi_);
            }
        }

        void drain_wake_pipe()
        {
            char buffer[128];

            for (;;) {
                const ssize_t result = read(
                    wake_read_fd_,
                    buffer,
                    sizeof(buffer)
                );

                if (result > 0) {
                    continue;
                }

                if (result < 0 && errno == EINTR) {
                    continue;
                }

                break;
            }
        }

        void set_state(TransportState value)
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_ = value;
        }

        void set_error(const std::string& error)
        {
            std::lock_guard<std::mutex> lock(state_mutex_);

            last_error_ = error;
            state_ = TransportState::ERROR;
        }

        bool pop_outgoing(WebSocketMessage& message)
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);

            if (outgoing_.empty()) {
                return false;
            }

            const std::size_t message_size =
            outgoing_.front().payload.size();

            message = std::move(outgoing_.front());
            outgoing_.pop_front();

            outgoing_bytes_ -= message_size;

            return true;
        }

        bool enqueue_incoming(WebSocketMessage message)
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);

            if (incoming_.size() >= config_.incoming_queue_capacity ||
                incoming_bytes_ > MAX_QUEUE_BYTES ||
                message.payload.size() >
                MAX_QUEUE_BYTES - incoming_bytes_) {
                return false;
                }

                const std::size_t message_size = message.payload.size();

            incoming_.push_back(std::move(message));
            incoming_bytes_ += message_size;

            return true;
        }

        bool wait_socket(curl_socket_t socket, short events)
        {
            pollfd descriptors[2] {};

            descriptors[0].fd = static_cast<int>(socket);
            descriptors[0].events = events;

            descriptors[1].fd = wake_read_fd_;
            descriptors[1].events = POLLIN;

            while (!stop_requested_.load()) {
                const int result = poll(
                    descriptors,
                    2,
                    static_cast<int>(config_.io_poll_interval_ms)
                );

                if (result < 0) {
                    if (errno == EINTR) {
                        continue;
                    }

                    set_error("WebSocket poll failed");
                    return false;
                }

                if (result == 0) {
                    continue;
                }

                if (descriptors[1].revents & POLLIN) {
                    drain_wake_pipe();

                    if (stop_requested_.load()) {
                        return false;
                    }
                }
                if (descriptors[0].revents &
                    (POLLERR | POLLHUP | POLLNVAL)) {
                    return false;
                }

                if (descriptors[0].revents & events) {
                    return true;
                }
            }

            return false;
        }

        bool send_message(
            CURL* curl,
            curl_socket_t socket,
            const WebSocketMessage& message)
        {
            const char* data = message.payload.empty()
            ? ""
            : message.payload.data();

            const std::size_t length = message.payload.size();

            const unsigned int frame_type =
            message.type == WebSocketMessageType::BINARY
            ? CURLWS_BINARY
            : CURLWS_TEXT;

            std::size_t offset = 0;
            bool first_call = true;

            while (first_call || offset < length) {
                first_call = false;

                if (stop_requested_.load()) {
                    return false;
                }

                std::size_t sent = 0;

                const CURLcode result = curl_ws_send(
                    curl,
                    data + offset,
                    length - offset,
                    &sent,
                    0,
                    frame_type
                );

                if (result == CURLE_AGAIN) {
                    if (!wait_socket(socket, POLLOUT)) {
                        return false;
                    }

                    continue;
                }

                if (result != CURLE_OK) {
                    set_error(curl_easy_strerror(result));
                    return false;
                }

                if (sent > length - offset) {
                    set_error("Invalid WebSocket send length");
                    return false;
                }

                offset += sent;

                if (offset < length && sent == 0) {
                    set_error("WebSocket send made no progress");
                    return false;
                }
            }

            return true;
        }

        bool receive_available(CURL* curl, curl_socket_t socket)
        {
            char buffer[IO_BUFFER_SIZE];

            for (;;) {
                std::size_t received = 0;
                const curl_ws_frame* metadata = nullptr;

                const CURLcode result = curl_ws_recv(
                    curl,
                    buffer,
                    sizeof(buffer),
                                                     &received,
                                                     &metadata
                );

                if (result == CURLE_AGAIN) {
                    return true;
                }

                if (result == CURLE_GOT_NOTHING) {
                    return false;
                }

                if (result != CURLE_OK) {
                    set_error(curl_easy_strerror(result));
                    return false;
                }

                if (metadata == nullptr) {
                    set_error("Missing WebSocket frame metadata");
                    return false;
                }

                const int flags = metadata->flags;

                if (metadata->bytesleft < 0) {
                    set_error("Invalid WebSocket frame length");
                    return false;
                }

                if (flags & CURLWS_CLOSE) {
                    return false;
                }

                if (flags & CURLWS_PING) {
                    const std::size_t remaining =
                        static_cast<std::size_t>(metadata->bytesleft);

                    if (received > 125 ||
                        ping_buffer_.size() > 125 - received ||
                        remaining > 125 - ping_buffer_.size() - received) {
                        set_error("WebSocket PING payload exceeds 125 bytes");
                        return false;
                    }

                    ping_buffer_.append(buffer, received);

                    if (remaining != 0) {
                        continue;
                    }

                    const char* payload = ping_buffer_.data();
                    const std::size_t payload_size = ping_buffer_.size();

                    std::size_t offset = 0;
                    bool first_call = true;

                    while (first_call || offset < payload_size) {
                        first_call = false;

                        std::size_t sent = 0;

                        const CURLcode pong_result = curl_ws_send(
                            curl,
                            payload + offset,
                            payload_size - offset,
                            &sent,
                            0,
                            CURLWS_PONG
                        );

                        if (sent > payload_size - offset) {
                            set_error("Invalid WebSocket PONG send length");
                            return false;
                        }

                        offset += sent;

                        if (pong_result == CURLE_AGAIN) {
                            if (!wait_socket(socket, POLLOUT)) {
                                return false;
                            }
                            continue;
                        }

                        if (pong_result != CURLE_OK) {
                            set_error(curl_easy_strerror(pong_result));
                            return false;
                        }

                        if (offset < payload_size && sent == 0) {
                            set_error("WebSocket PONG send made no progress");
                            return false;
                        }
                    }

                    ping_buffer_.clear();
                    continue;
                }

                if (flags & CURLWS_PONG) {
                    continue;
                }

                WebSocketMessageType frame_type;

                if (flags & CURLWS_BINARY) {
                    frame_type = WebSocketMessageType::BINARY;
                } else if (flags & CURLWS_TEXT) {
                    frame_type = WebSocketMessageType::TEXT;
                } else {
                    set_error("Unsupported WebSocket frame type");
                    return false;
                }

                const std::size_t remaining =
                static_cast<std::size_t>(metadata->bytesleft);

                if (received > config_.max_message_bytes ||
                    receive_buffer_.size() >
                    config_.max_message_bytes - received) {
                    set_error("Incoming WebSocket message exceeds limit");
                return false;
                    }

                    if (!receive_message_active_) {
                        receive_message_type_ = frame_type;
                        receive_message_active_ = true;
                    } else if (receive_message_type_ != frame_type) {
                        set_error("WebSocket message type changed between fragments");
                        return false;
                    }

                    receive_buffer_.append(buffer, received);

                    if (remaining >
                        config_.max_message_bytes - receive_buffer_.size()) {
                        set_error("Incoming WebSocket frame exceeds limit");
                    return false;
                        }

                        if (remaining > 0 && received == 0) {
                            set_error("WebSocket data frame made no progress");
                            return false;
                        }

                        if (remaining == 0 && !(flags & CURLWS_CONT)) {
                            WebSocketMessage message;
                            message.type = receive_message_type_;
                            message.payload = std::move(receive_buffer_);

                            if (!enqueue_incoming(std::move(message))) {
                                set_error("Incoming message queue is full");
                                return false;
                            }

                            receive_buffer_.clear();
                            receive_message_active_ = false;
                        }
            }
        }

        void send_close(CURL* curl, curl_socket_t socket)
        {
            for (int attempt = 0; attempt < 3; ++attempt) {
                std::size_t sent = 0;

                const CURLcode result = curl_ws_send(
                    curl,
                    "",
                    0,
                    &sent,
                    0,
                    CURLWS_CLOSE
                );

                if (result == CURLE_OK) {
                    return;
                }

                if (result != CURLE_AGAIN) {
                    return;
                }

                pollfd descriptor {};
                descriptor.fd = static_cast<int>(socket);
                descriptor.events = POLLOUT;

                int poll_result;

                do {
                    poll_result = poll(&descriptor, 1, 100);
                } while (poll_result < 0 && errno == EINTR);

                if (poll_result <= 0 ||
                    descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    return;
                    }
            }

        }

        bool configure_easy(CURL* curl, char* error_buffer)
        {
            return
            curl_easy_setopt(
                curl,
                CURLOPT_URL,
                config_.url.c_str()
            ) == CURLE_OK &&

            curl_easy_setopt(
                curl,
                CURLOPT_CONNECT_ONLY,
                2L
            ) == CURLE_OK &&

            curl_easy_setopt(
                curl,
                CURLOPT_WS_OPTIONS,
                CURLWS_NOAUTOPONG
            ) == CURLE_OK &&

            curl_easy_setopt(
                curl,
                CURLOPT_CONNECTTIMEOUT_MS,
                config_.connect_timeout_ms
            ) == CURLE_OK &&

            curl_easy_setopt(
                curl,
                CURLOPT_NOSIGNAL,
                1L
            ) == CURLE_OK &&

            curl_easy_setopt(
                curl,
                CURLOPT_TCP_KEEPALIVE,
                1L
            ) == CURLE_OK &&

            curl_easy_setopt(
                curl,
                CURLOPT_SSL_VERIFYPEER,
                config_.verify_tls ? 1L : 0L
            ) == CURLE_OK &&

            curl_easy_setopt(
                curl,
                CURLOPT_SSL_VERIFYHOST,
                config_.verify_tls ? 2L : 0L
            ) == CURLE_OK &&

            curl_easy_setopt(
                curl,
                CURLOPT_ERRORBUFFER,
                error_buffer
            ) == CURLE_OK;
        }

                bool connect_multi(
            CURLM* multi,
            CURL* curl,
            char* error_buffer,
            bool& added_to_multi)
        {
            CURLMcode multi_result = curl_multi_add_handle(multi, curl);

            if (multi_result != CURLM_OK) {
                set_error(curl_multi_strerror(multi_result));
                return false;
            }

            added_to_multi = true;

            bool completed = false;
            CURLcode transfer_result = CURLE_OK;

            while (!stop_requested_.load()) {
                int running_handles = 0;

                do {
                    multi_result = curl_multi_perform(
                        multi,
                        &running_handles
                    );
                } while (multi_result == CURLM_CALL_MULTI_PERFORM);

                if (multi_result != CURLM_OK) {
                    set_error(curl_multi_strerror(multi_result));
                    return false;
                }

                int messages_left = 0;
                CURLMsg* message = nullptr;

                while ((message = curl_multi_info_read(
                    multi,
                    &messages_left
                )) != nullptr) {
                    if (message->msg == CURLMSG_DONE &&
                        message->easy_handle == curl) {
                        transfer_result = message->data.result;
                    completed = true;
                        }
                }

                if (completed) {
                    if (transfer_result != CURLE_OK) {
                        set_error(
                            error_buffer[0] != '\0'
                        ? error_buffer
                        : curl_easy_strerror(transfer_result)
                        );

                        return false;
                    }

                    return true;
                }

                if (running_handles == 0) {
                    set_error("WebSocket connection ended before handshake");
                    return false;
                }

                int descriptors = 0;

                multi_result = curl_multi_poll(
                    multi,
                    nullptr,
                    0,
                    static_cast<int>(config_.io_poll_interval_ms),
                                               &descriptors
                );

                if (multi_result != CURLM_OK) {
                    set_error(curl_multi_strerror(multi_result));
                    return false;
                }
            }

            return false;
        }

        void run()
        {
            CURL* curl = nullptr;
            CURLM* multi = nullptr;

            bool added_to_multi = false;
            bool published_multi = false;
            bool connected = false;

            const auto mark_worker_error = [](
                Impl* self,
                const char* message
            ) noexcept {
                try {
                    self->set_error(message);
                } catch (...) {
                    try {
                        self->set_state(TransportState::ERROR);
                    } catch (...) {
                        // Do not allow error reporting to escape the worker.
                    }
                }
            };

            const auto cleanup = [&]() noexcept {
                bool remove_succeeded = true;

                if (added_to_multi && multi != nullptr &&
                    curl != nullptr) {
                    const CURLMcode result =
                        curl_multi_remove_handle(multi, curl);

                    if (result != CURLM_OK) {
                        remove_succeeded = false;
                        mark_worker_error(
                            this,
                            curl_multi_strerror(result)
                        );
                    }

                    added_to_multi = false;
                }

                if (published_multi && multi != nullptr) {
                    try {
                        clear_multi(multi);
                    } catch (...) {
                        mark_worker_error(
                            this,
                            "Unable to clear active WebSocket multi handle"
                        );
                    }

                    published_multi = false;
                }

                if (!remove_succeeded && multi != nullptr) {
                    curl_multi_cleanup(multi);
                    multi = nullptr;
                }

                if (curl != nullptr) {
                    curl_easy_cleanup(curl);
                    curl = nullptr;
                }

                if (multi != nullptr) {
                    curl_multi_cleanup(multi);
                    multi = nullptr;
                }
            };

            try {
                curl = curl_easy_init();

                if (curl == nullptr) {
                    set_error("curl_easy_init failed");
                } else {
                    multi = curl_multi_init();

                    if (multi == nullptr) {
                        set_error("curl_multi_init failed");
                    } else {
                        publish_multi(multi);
                        published_multi = true;

                        char error_buffer[CURL_ERROR_SIZE] = {};

                        if (!configure_easy(curl, error_buffer)) {
                            set_error(
                                "Unable to configure libcurl WebSocket options"
                            );
                        } else {
                            if (!stop_requested_.load()) {
                                connected = connect_multi(
                                    multi,
                                    curl,
                                    error_buffer,
                                    added_to_multi
                                );
                            }

                            curl_socket_t socket = CURL_SOCKET_BAD;

                            if (connected &&
                                !stop_requested_.load()) {
                                if (curl_easy_getinfo(
                                    curl,
                                    CURLINFO_ACTIVESOCKET,
                                    &socket
                                ) != CURLE_OK ||
                                    socket == CURL_SOCKET_BAD) {
                                    set_error(
                                        "Unable to obtain WebSocket socket"
                                    );
                                    connected = false;
                                }
                            }

                            if (connected &&
                                !stop_requested_.load()) {
                                set_state(TransportState::CONNECTED);
                            }

                            while (connected &&
                                !stop_requested_.load()) {
                                WebSocketMessage outgoing_message;

                                if (pop_outgoing(outgoing_message)) {
                                    if (!send_message(
                                        curl,
                                        socket,
                                        outgoing_message
                                    )) {
                                        if (!stop_requested_.load()) {
                                            connected = false;
                                        }

                                        break;
                                    }
                                }

                                if (stop_requested_.load()) {
                                    break;
                                }

                                pollfd descriptors[2] {};

                                descriptors[0].fd =
                                    static_cast<int>(socket);
                                descriptors[0].events = POLLIN;

                                descriptors[1].fd = wake_read_fd_;
                                descriptors[1].events = POLLIN;

                                const int poll_result = poll(
                                    descriptors,
                                    2,
                                    static_cast<int>(
                                        config_.io_poll_interval_ms
                                    )
                                );

                                if (poll_result < 0) {
                                    if (errno == EINTR) {
                                        continue;
                                    }

                                    set_error("WebSocket poll failed");
                                    connected = false;
                                    break;
                                }

                                if (poll_result == 0) {
                                    continue;
                                }

                                if (descriptors[1].revents & POLLIN) {
                                    drain_wake_pipe();

                                    if (stop_requested_.load()) {
                                        break;
                                    }
                                }

                                if (descriptors[1].revents &
                                    (POLLERR | POLLHUP | POLLNVAL)) {
                                    set_error(
                                        "WebSocket wake pipe error"
                                    );
                                    connected = false;
                                    break;
                                }

                                if (descriptors[0].revents & POLLIN) {
                                    const bool receive_ok =
                                        receive_available(curl, socket);

                                    if (!receive_ok) {
                                        if (!stop_requested_.load() &&
                                            state() !=
                                                TransportState::ERROR) {
                                            set_error(
                                                "WebSocket peer closed the connection"
                                            );
                                        }

                                        connected = false;
                                        break;
                                    }
                                }

                                if (descriptors[0].revents &
                                    (POLLERR | POLLNVAL)) {
                                    set_error("WebSocket socket error");
                                    connected = false;
                                    break;
                                }

                                if (descriptors[0].revents & POLLHUP) {
                                    if (!stop_requested_.load() &&
                                        state() !=
                                            TransportState::ERROR) {
                                        set_error(
                                            "WebSocket peer disconnected unexpectedly"
                                        );
                                    }

                                    connected = false;
                                    break;
                                }
                            }

                            if (stop_requested_.load() &&
                                connected &&
                                socket != CURL_SOCKET_BAD) {
                                send_close(curl, socket);
                            }
                        }
                    }
                }
            } catch (const std::exception&) {
                mark_worker_error(
                    this,
                    "WebSocket worker exception"
                );
            } catch (...) {
                mark_worker_error(
                    this,
                    "Unknown WebSocket worker exception"
                );
            }

            cleanup();

            try {
                if (state() != TransportState::ERROR) {
                    set_state(TransportState::STOPPED);
                }
            } catch (...) {
                // State finalization must not escape the worker.
            }
        }

        WebSocketConfig config_;

        mutable std::mutex state_mutex_;
        mutable std::mutex queue_mutex_;
        std::mutex lifecycle_mutex_;

        std::mutex multi_mutex_;
        CURLM* active_multi_ = nullptr;

        TransportState state_ = TransportState::STOPPED;
        std::string last_error_;

        std::deque<WebSocketMessage> outgoing_;
        std::deque<WebSocketMessage> incoming_;

        std::size_t outgoing_bytes_ = 0;
        std::size_t incoming_bytes_ = 0;

        std::string receive_buffer_;
        std::string ping_buffer_;

        WebSocketMessageType receive_message_type_ =
        WebSocketMessageType::TEXT;

        bool receive_message_active_ = false;

        std::atomic<bool> stop_requested_ {false};
        std::thread worker_;
        int wake_read_fd_ = -1;
        int wake_write_fd_ = -1;
    };

    WebSocketTransport::WebSocketTransport(WebSocketConfig config)
    : impl_(new Impl(std::move(config)))
    {
    }

    WebSocketTransport::~WebSocketTransport() = default;

    bool WebSocketTransport::start()
    {
        return impl_->start();
    }

    void WebSocketTransport::stop()
    {
        impl_->stop();
    }

    bool WebSocketTransport::send_text(const std::string& message)
    {
        return impl_->send_text(message);
    }

    bool WebSocketTransport::send_binary(const std::string& message)
    {
        return impl_->send_binary(message);
    }

    bool WebSocketTransport::try_receive(WebSocketMessage& message)
    {
        return impl_->try_receive(message);
    }

    bool WebSocketTransport::try_receive(std::string& message)
    {
        return impl_->try_receive(message);
    }

    TransportState WebSocketTransport::state() const
    {
        return impl_->state();
    }

    bool WebSocketTransport::is_connected() const
    {
        return impl_->is_connected();
    }

    std::string WebSocketTransport::last_error() const
    {
        return impl_->last_error();
    }

} // namespace nakama
