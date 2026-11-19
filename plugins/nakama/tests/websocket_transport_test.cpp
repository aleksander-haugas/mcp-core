#include "nakama/websocket_transport.hpp"
#include "mock_websocket_server.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <cstdio>

using namespace std::chrono_literals;
using namespace nakama;

namespace {

bool wait_connected(
    const WebSocketTransport& transport,
    const MockWebSocketServer& server,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (transport.is_connected() && server.connected()) {
            return true;
        }

        if (transport.state() == TransportState::ERROR) {
            return false;
        }

        std::this_thread::sleep_for(10ms);
    }

    return false;
}

bool wait_received(
    WebSocketTransport& transport,
    WebSocketMessage& message,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (transport.try_receive(message)) {
            return true;
        }

        if (transport.state() == TransportState::ERROR) {
            return false;
        }

        std::this_thread::sleep_for(10ms);
    }

    return false;
}

bool check(bool condition, const char* description)
{
    std::cout
        << (condition ? "[PASS] " : "[FAIL] ")
        << description << '\n';

    return condition;
}

} // namespace

int main()
{
    int failures = 0;

    MockWebSocketServer server;

    if (!check(server.start(), "Start local WebSocket server")) {
        return 1;
    }

    WebSocketConfig config;
    config.url = "ws://127.0.0.1:" +
        std::to_string(server.port()) + "/";
    config.connect_timeout_ms = 5000;
    config.io_poll_interval_ms = 50;
    config.max_message_bytes = 1024 * 1024;
    config.outgoing_queue_capacity = 16;
    config.incoming_queue_capacity = 16;
    config.verify_tls = true;

    WebSocketTransport transport(config);

    if (!check(transport.start(), "Start WebSocket transport")) {
        server.stop();
        return 1;
    }

    const bool connected = wait_connected(
        transport,
        server,
        5s
    );

    if (!check(connected, "Complete HTTP WebSocket handshake")) {
        ++failures;
        std::cerr << "Transport error: "
                  << transport.last_error() << '\n';
    }

    if (connected) {
        const std::string text_payload =
            R"({"type":"transport.test","value":"text"})";

        const std::string binary_payload{
            '\x00', '\x01', '\x02', '\x7f',
            '\x80', '\xfe', '\xff'
        };

        const bool text_queued = transport.send_text(text_payload);

        if (!check(text_queued, "Queue outgoing TEXT message")) {
            ++failures;
        }

        std::string received;
        bool received_binary = false;

        const bool text_received = server.wait_for_message(
            received,
            received_binary,
            3000
        );

        const bool text_valid =
            text_received &&
            !received_binary &&
            received == text_payload;

        if (!check(text_valid, "Server receives correct TEXT frame")) {
            ++failures;
        }

        const bool binary_queued =
            transport.send_binary(binary_payload);

        if (!check(binary_queued, "Queue outgoing BINARY message")) {
            ++failures;
        }

        received.clear();
        received_binary = false;

        const bool binary_received = server.wait_for_message(
            received,
            received_binary,
            3000
        );

        const bool binary_valid =
            binary_received &&
            received_binary &&
            received == binary_payload;

        if (!check(
            binary_valid,
            "Server receives correct BINARY frame"
        )) {
            ++failures;
        }

        const std::string server_text =
            R"({"reply":"text"})";

        const bool server_text_sent =
            server.send_text(server_text);

        if (!check(server_text_sent, "Server sends TEXT frame")) {
            ++failures;
        }

        WebSocketMessage incoming;

        const bool client_text_received =
            wait_received(transport, incoming, 3s);

        const bool client_text_valid =
            client_text_received &&
            incoming.type == WebSocketMessageType::TEXT &&
            incoming.payload == server_text;

        if (!check(
            client_text_valid,
            "Client receives correct TEXT message"
        )) {
            ++failures;
        }

        const std::string server_binary{
            '\x00', '\x10', '\x20', '\x80', '\xff'
        };

        const bool server_binary_sent =
            server.send_binary(server_binary);

        if (!check(
            server_binary_sent,
            "Server sends BINARY frame"
        )) {
            ++failures;
        }

        incoming = WebSocketMessage{};

        const bool client_binary_received =
            wait_received(transport, incoming, 3s);

        const bool client_binary_valid =
            client_binary_received &&
            incoming.type == WebSocketMessageType::BINARY &&
            incoming.payload == server_binary;

        if (!check(
            client_binary_valid,
            "Client receives correct BINARY message"
        )) {
            ++failures;
        }

        const std::string fragmented_text =
            "fragmented-text-message-0123456789";

        const bool fragmented_text_sent =
            server.send_fragmented_text(fragmented_text, 5);

        if (!check(
            fragmented_text_sent,
            "Server sends fragmented TEXT message"
        )) {
            ++failures;
        }

        incoming = WebSocketMessage{};

        const bool fragmented_text_received =
            wait_received(transport, incoming, 3s);

        const bool fragmented_text_valid =
            fragmented_text_received &&
            incoming.type == WebSocketMessageType::TEXT &&
            incoming.payload == fragmented_text;

        if (!check(
            fragmented_text_valid,
            "Client reconstructs fragmented TEXT message"
        )) {
            ++failures;
        }

        const std::string fragmented_binary{
            '\x00', '\x01', '\x7f', '\x80', '\xff',
            '\x10', '\x20', '\x30', '\x40', '\x50',
            '\xaa', '\xbb', '\xcc'
        };

        const bool fragmented_binary_sent =
            server.send_fragmented_binary(fragmented_binary, 3);

        if (!check(
            fragmented_binary_sent,
            "Server sends fragmented BINARY message"
        )) {
            ++failures;
        }

        incoming = WebSocketMessage{};

        const bool fragmented_binary_received =
            wait_received(transport, incoming, 3s);

        const bool fragmented_binary_valid =
            fragmented_binary_received &&
            incoming.type == WebSocketMessageType::BINARY &&
            incoming.payload == fragmented_binary;

        if (!check(
            fragmented_binary_valid,
            "Client reconstructs fragmented BINARY message"
        )) {
            ++failures;
        }

        const std::string ping_interleaved_text =
            "text-with-interleaved-ping-0123456789";

        const std::string ping_payload = "heartbeat-check";

        const bool interleaved_ping_sent =
            server.send_fragmented_text_with_ping(
                ping_interleaved_text,
                ping_payload,
                5
            );

        if (!check(
            interleaved_ping_sent,
            "Server sends fragmented TEXT with interleaved PING"
        )) {
            ++failures;
        }

        incoming = WebSocketMessage{};

        const bool interleaved_text_received =
            wait_received(transport, incoming, 3s);

        const bool interleaved_text_valid =
            interleaved_text_received &&
            incoming.type == WebSocketMessageType::TEXT &&
            incoming.payload == ping_interleaved_text;

        if (!check(
            interleaved_text_valid,
            "Client reconstructs TEXT across interleaved PING"
        )) {
            ++failures;
        }

        std::string received_pong;

        const bool pong_received =
            server.wait_for_pong(received_pong, 3000);

        if (!check(
            pong_received && received_pong == ping_payload,
            "Client automatically responds to interleaved PING"
        )) {
            ++failures;
        }

        const std::string standalone_ping = "standalone-heartbeat";

        const bool standalone_ping_sent =
            server.send_ping(standalone_ping);

        if (!check(
            standalone_ping_sent,
            "Server sends standalone PING"
        )) {
            ++failures;
        }

        // The standalone PONG must arrive without outgoing data.
        std::string standalone_pong;

        const bool standalone_pong_received =
            server.wait_for_pong(standalone_pong, 3000);

        if (!check(
            standalone_pong_received &&
            standalone_pong == standalone_ping,
            "Standalone PING receives matching PONG without outgoing data"
        )) {
            ++failures;
        }

        const std::string trigger_message =
            "standalone-ping-send-trigger";

        const bool trigger_queued =
            transport.send_text(trigger_message);

        if (!check(
            trigger_queued,
            "Queue TEXT after standalone PING"
        )) {
            ++failures;
        }

            std::string trigger_received;
            bool trigger_binary = false;

            const bool trigger_message_received =
            server.wait_for_message(
                trigger_received,
                trigger_binary,
                3000
            );

            if (!check(
                trigger_message_received &&
                !trigger_binary &&
                trigger_received == trigger_message,
                "Server receives TEXT sent after standalone PING"
            )) {
                ++failures;
            }

        const std::string oversized(
            config.max_message_bytes + 1,
            'X'
        );

        if (!check(
            !transport.send_text(oversized),
            "Reject outgoing message exceeding configured limit"
        )) {
            ++failures;
        }
    }

    transport.stop();

    if (!check(
        transport.state() == TransportState::STOPPED,
        "Stop transport cleanly"
    )) {
        ++failures;
    }

    server.stop();

    if (!check(
        !server.connected(),
        "Stop local WebSocket server"
    )) {
        ++failures;
    }

    std::cout << "\nFailures: " << failures << '\n';

    return failures == 0 ? 0 : 1;
}
