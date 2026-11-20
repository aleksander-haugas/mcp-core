#include "mcp/plugin_manager.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include <thread>

namespace {

struct ReceivedMessage {
    bool received = false;
    std::string plugin_id;
    std::string transport_id;
    std::string principal;
    std::vector<std::uint8_t> data;
    int received_count = 0;
    bool empty_payload_received = false;
};

int failures = 0;

void check(bool condition, const char* description)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ")
              << description << '\n';

    if (!condition) {
        ++failures;
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr
            << "Usage: " << argv[0]
            << " /path/to/plugin-directory\n";
        return 2;
    }

    ReceivedMessage received;
    std::mutex mutex;
    std::condition_variable condition;

    mcp::PluginManager manager;

    manager.set_authenticated_message_handler(
        [&](const mcp::AuthenticatedPluginMessage& message) {
            {
                std::lock_guard<std::mutex> lock(mutex);

                ++received.received_count;

                if (received.received_count == 1) {
                    received.received = true;
                    received.plugin_id = message.plugin_id;
                    received.transport_id = message.transport_id;
                    received.principal = message.authenticated_principal;
                    received.data = message.data;
                } else if (received.received_count == 2) {
                    received.empty_payload_received = message.data.empty();
                }
            }

            condition.notify_one();
        }
    );

    check(
        manager.load_directory(argv[1], "{}"),
        "Plugin directory loaded"
    );

    check(
        manager.plugin_count() == 1,
        "Exactly one test plugin loaded"
    );

    check(
        manager.tool_count() == 1,
        "Exactly one test tool registered"
    );

    std::string result;
    std::string error;

    const bool executed = manager.execute_tool(
        "nakama_test_publish",
        "{}",
        result,
        error
    );

    check(
        executed,
        executed ? "Authenticated publication tool executed"
                 : error.c_str()
    );

    check(
        result == "{\"published\":true}",
        "Plugin reported successful publication"
    );

    bool arrived = false;

    {
        std::unique_lock<std::mutex> lock(mutex);

        arrived = condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] {
                return received.received_count >= 2;
            }
        );
    }

    check(
        arrived,
        "Authenticated message delivered to Core"
    );

    if (arrived) {
        check(
            received.plugin_id == "nakama",
            "Plugin identity preserved"
        );

        check(
            received.transport_id == "nakama",
            "Transport identity preserved"
        );

        check(
            received.principal == "nakama-user-42",
            "Authenticated principal preserved"
        );

        const std::string expected = "authenticated-payload-original";

        const std::string actual(
            received.data.begin(),
            received.data.end()
        );

        check(
            actual == expected,
            "Payload copied before publish returned"
        );

        check(
            received.empty_payload_received,
            "Empty authenticated payload delivered successfully"
        );
    }

    // Give the worker time to deliver any incorrectly queued messages.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    {
        std::lock_guard<std::mutex> lock(mutex);

        check(
            received.received_count == 2,
            "Exactly two valid publications delivered; invalid ones rejected"
        );
    }

    manager.clear_authenticated_message_handler();
    manager.shutdown();

    check(
        manager.plugin_count() == 0 &&
        manager.tool_count() == 0,
        "Plugin shutdown completed and registries are empty"
    );

    std::cout << "\nFailures: " << failures << '\n';

    if (failures == 0) {
        std::cout << "PLUGIN MANAGER ASYNC V2 TEST: PASS\n";
        return 0;
    }

    std::cout << "PLUGIN MANAGER ASYNC V2 TEST: FAIL\n";
    return 1;
}
