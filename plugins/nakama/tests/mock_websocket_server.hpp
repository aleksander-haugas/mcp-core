#ifndef NAKAMA_TESTS_MOCK_WEBSOCKET_SERVER_HPP
#define NAKAMA_TESTS_MOCK_WEBSOCKET_SERVER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

class MockWebSocketServer {
public:
    MockWebSocketServer();
    ~MockWebSocketServer();

    MockWebSocketServer(const MockWebSocketServer&) = delete;
    MockWebSocketServer& operator=(const MockWebSocketServer&) = delete;

    bool start();
    void stop();

    std::uint16_t port() const;
    bool connected() const;

    bool send_text(const std::string& message);
    bool send_binary(const std::string& message);

    bool send_fragmented_text(
        const std::string& message,
        std::size_t fragment_size = 0
    );

    bool send_fragmented_binary(
        const std::string& message,
        std::size_t fragment_size = 0
    );

    bool send_fragmented_text_with_ping(
        const std::string& message,
        const std::string& ping_payload,
        std::size_t fragment_size = 0
    );

    bool send_ping(const std::string& payload = {});
    bool send_close(
        std::uint16_t code = 1000,
        const std::string& reason = {}
    );

    bool wait_for_message(
        std::string& message,
        bool& binary,
        int timeout_ms
    );

    bool wait_for_pong(std::string& payload, int timeout_ms);
    bool wait_for_close(std::string& payload, int timeout_ms);

private:
    struct ReceivedMessage {
        std::string payload;
        bool binary = false;
    };

    void run();

    bool perform_handshake(int client_fd);
    bool receive_client_frames(int client_fd);

    bool send_server_frame(
        int client_fd,
        unsigned char opcode,
        bool final_frame,
        const std::string& payload
    );

    bool send_data_frame(
        int client_fd,
        const std::string& payload,
        bool binary
    );

    bool send_server_frame_locked(
        int client_fd,
        unsigned char opcode,
        bool final_frame,
        const std::string& payload
    );

    bool send_fragmented_message(
        int client_fd,
        const std::string& message,
        unsigned char opcode,
        std::size_t fragment_size,
        const std::string* interleaved_ping
    );

    bool wait_for_control_payload(
        std::deque<std::string>& queue,
        std::string& payload,
        int timeout_ms
    );

    int listen_fd_ = -1;
    std::atomic<int> client_fd_{-1};
    std::uint16_t port_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    std::mutex send_mutex_;
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;

    std::deque<ReceivedMessage> incoming_;
    std::deque<std::string> pongs_;
    std::deque<std::string> closes_;

    std::thread worker_;
};

#endif
