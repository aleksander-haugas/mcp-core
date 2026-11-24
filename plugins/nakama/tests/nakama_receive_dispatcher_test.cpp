#include "nakama/receive_dispatcher.hpp"
#include "mock_websocket_server.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <json-c/json.h>

using namespace std::chrono_literals;
using namespace nakama;

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ")
              << description << '\n';

    if (!condition) {
        ++failures;
    }
}

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

bool wait_handler_count(
    std::mutex& mutex,
    std::condition_variable& condition,
    std::size_t& count,
    std::size_t expected,
    std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex);

    return condition.wait_for(
        lock,
        timeout,
        [&count, expected] {
            return count >= expected;
        }
    );
}

bool check_correlated_response(
    ReceiveDispatcher& dispatcher,
    MockWebSocketServer& server,
    const WebSocketTransport& transport,
    const std::string& cid,
    const std::string& payload,
    const char* description)
{
    if (!dispatcher.register_pending(cid)) {
        check(false, description);
        return false;
    }

    if (!server.send_text(payload)) {
        std::cerr << "[DIAG] send_text failed for CID "
                  << cid << std::endl;
        dispatcher.cancel_pending(cid);
        check(false, description);
        return false;
    }

    WebSocketMessage response;

    const bool received = dispatcher.wait_for_response(
        cid,
        3s,
        response
    );

    if (!received) {
        std::cerr << "[DIAG] No correlated response for CID "
                  << cid
                  << "; pending_count=" << dispatcher.pending_count()
                  << "; transport_state="
                  << static_cast<int>(transport.state())
                  << "; transport_error=" << transport.last_error()
                  << std::endl;
        check(false, description);
        return false;
    }

    const bool correct_type =
        response.type == WebSocketMessageType::TEXT;
    const bool correct_payload = response.payload == payload;

    if (!correct_type || !correct_payload) {
        std::cerr << "[DIAG] Response mismatch for CID " << cid
                  << "; type=" << static_cast<int>(response.type)
                  << "; received_size=" << response.payload.size()
                  << "; expected_size=" << payload.size()
                  << std::endl;
    }

    const bool valid = correct_type && correct_payload;
    check(valid, description);
    return valid;
}

} // namespace

int main()
{
    MockWebSocketServer server;

    const bool server_started = server.start();
    check(server_started, "Start mock WebSocket server");

    if (!server_started) {
        return 1;
    }

    WebSocketConfig config;
    config.url = "ws://127.0.0.1:" +
        std::to_string(server.port()) + "/";
    config.connect_timeout_ms = 5000;
    config.io_poll_interval_ms = 50;
    config.max_message_bytes = 4 * 1024 * 1024;
    config.outgoing_queue_capacity = 8;
    config.incoming_queue_capacity = 8;
    config.verify_tls = true;

    auto transport = std::make_shared<WebSocketTransport>(config);

    check(transport->start(), "Start WebSocket transport");

    if (!wait_connected(*transport, server, 5s)) {
        check(false, "Connect transport to mock server");
        transport->stop();
        server.stop();
        return 1;
    }

    check(true, "Connect transport to mock server");

    std::mutex handler_mutex;
    std::condition_variable handler_condition;
    std::size_t handler_count = 0;
    std::size_t handler_last_size = 0;

    ReceiveDispatcher dispatcher(
        transport,
        [&handler_mutex, &handler_condition, &handler_count,
         &handler_last_size](const WebSocketMessage& message) {
            {
                std::lock_guard<std::mutex> lock(handler_mutex);
                ++handler_count;
                handler_last_size = message.payload.size();
            }
            handler_condition.notify_all();
        }
    );

    check(dispatcher.start(), "Start receive dispatcher");

    check_correlated_response(
        dispatcher,
        server,
        *transport,
        "pong-1",
        R"({"cid":"pong-1","pong":{}})",
        "Correlate pong response"
    );

    check_correlated_response(
        dispatcher,
        server,
        *transport,
        "rpc-1",
        R"({"cid":"rpc-1","rpc":{"id":"mcp_agents_send","payload":"{}"}})",
        "Correlate RPC response"
    );

    check_correlated_response(
        dispatcher,
        server,
        *transport,
        "error-1",
        R"({"cid":"error-1","error":{"code":13,"message":"permission denied"}})",
        "Correlate RPC error response"
    );

    // A valid response with an unknown CID must reach the message handler.
    check(
        server.send_text(R"({"cid":"unknown","rpc":{"id":"test"}})"),
        "Send uncorrelated RPC response"
    );

    check(
        wait_handler_count(
            handler_mutex,
            handler_condition,
            handler_count,
            1,
            3s
        ),
        "Forward uncorrelated response to message handler"
    );

    // Invalid JSON must not resolve a pending request.
    check(
        dispatcher.register_pending("invalid-1"),
        "Register pending request for malformed response test"
    );

    check(
        server.send_text(R"({"cid":"invalid-1","rpc":)"),
        "Send malformed JSON response"
    );

    check(
        wait_handler_count(
            handler_mutex,
            handler_condition,
            handler_count,
            2,
            3s
        ),
        "Forward malformed response to message handler"
    );

    WebSocketMessage invalid_response;
    check(
        !dispatcher.wait_for_response(
            "invalid-1",
            100ms,
            invalid_response
        ),
        "Malformed JSON does not resolve pending request"
    );

    // A response larger than the previous 1 MiB parser limit.
    const std::string large_payload(2 * 1024 * 1024, 'x');

    const std::string large_response =
    std::string("{\"cid\":\"large-1\",\"rpc\":{\"id\":\"history\",\"payload\":\"") +
    large_payload +
    "\"}}";

    check_correlated_response(
        dispatcher,
        server,
        *transport,
        "large-1",
        large_response,
        "Correlate response larger than 1 MiB"
    );

    {
        std::lock_guard<std::mutex> lock(handler_mutex);
        std::cerr << "[DIAG] handler_count=" << handler_count
                  << "; handler_last_size=" << handler_last_size
                  << "; expected_large_size=" << large_response.size()
                  << std::endl;
    }

    check(
        dispatcher.pending_count() == 0,
        "Timed-out malformed request is removed from pending requests"
    );

    dispatcher.stop();
    transport->stop();
    server.stop();

    check(
        transport->state() == TransportState::STOPPED,
        "Stop transport cleanly"
    );

    std::cout << "\nFailures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
