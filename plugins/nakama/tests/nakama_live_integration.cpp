#include "nakama/websocket_transport.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace nakama;

int main()
{
    std::ifstream token_file("/tmp/mcp-nakama-token");

    if (!token_file) {
        std::cerr << "FAIL: JWT file unavailable\n";
        return 1;
    }

    std::string token;
    std::getline(token_file, token);

    if (token.empty()) {
        std::cerr << "FAIL: Empty JWT\n";
        return 1;
    }

    WebSocketConfig config;
    config.url =
        "ws://192.168.100.3:7350/ws?token=" +
        token + "&format=json&lang=en";

    config.connect_timeout_ms = 10000;
    config.io_poll_interval_ms = 50;
    config.max_message_bytes = 1024 * 1024;
    config.outgoing_queue_capacity = 16;
    config.incoming_queue_capacity = 16;

    WebSocketTransport transport(config);

    if (!transport.start()) {
        std::cerr << "FAIL: Transport start: "
                  << transport.last_error() << '\n';
        return 1;
    }

    bool connected = false;

    const auto connect_deadline =
        std::chrono::steady_clock::now() + 10s;

    while (std::chrono::steady_clock::now() < connect_deadline) {
        const auto state = transport.state();

        if (state == TransportState::CONNECTED) {
            connected = true;
            break;
        }

        if (state == TransportState::ERROR ||
            state == TransportState::STOPPED) {
            break;
        }

        std::this_thread::sleep_for(20ms);
    }

    if (!connected) {
        std::cerr << "FAIL: WebSocket authentication/handshake: "
                  << transport.last_error() << '\n';
        transport.stop();
        return 1;
    }

    std::cout << "PASS: Authenticated WebSocket connection\n";

    const std::string cid = "mcp-core-ping-001";
    const std::string ping =
        R"({"cid":"mcp-core-ping-001","ping":{}})";

    if (!transport.send_text(ping)) {
        std::cerr << "FAIL: Could not queue Nakama ping\n";
        transport.stop();
        return 1;
    }

    std::cout << "PASS: Application ping queued\n";

    bool pong_received = false;

    const auto pong_deadline =
        std::chrono::steady_clock::now() + 10s;

    while (std::chrono::steady_clock::now() < pong_deadline) {
        WebSocketMessage message;

        if (transport.try_receive(message)) {
            if (message.type == WebSocketMessageType::TEXT &&
                message.payload.find("\"pong\"") != std::string::npos &&
                message.payload.find(cid) != std::string::npos) {
                pong_received = true;
                break;
            }

        }

        if (transport.state() == TransportState::ERROR) {
            break;
        }

        std::this_thread::sleep_for(20ms);
    }

    if (!pong_received) {
        std::cerr << "FAIL: Nakama pong not received: "
                  << transport.last_error() << '\n';
        transport.stop();
        return 1;
    }

    std::cout << "PASS: Nakama pong received with matching CID\n";

    transport.stop();

    std::cout << "PASS: Transport stopped cleanly\n";
    return 0;
}
