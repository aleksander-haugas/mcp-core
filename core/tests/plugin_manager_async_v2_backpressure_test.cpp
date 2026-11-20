#include "mcp/plugin_manager.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>

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

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0]
                  << " /path/to/plugin-directory\n";
        return 2;
    }

    std::mutex mutex;
    std::condition_variable condition;

    bool callback_entered = false;
    bool release_callback = false;
    int received = 0;
    int invalid_messages = 0;

    mcp::PluginManager manager;

    manager.set_authenticated_message_handler(
        [&](const mcp::AuthenticatedPluginMessage& message) {
            std::unique_lock<std::mutex> lock(mutex);

            ++received;

            if (message.plugin_id != "nakama" ||
                message.transport_id != "nakama" ||
                message.authenticated_principal !=
                    "backpressure-test-user") {
                ++invalid_messages;
            }

            if (received == 1) {
                callback_entered = true;
                condition.notify_all();

                condition.wait(lock, [&] {
                    return release_callback;
                });
            }
        }
    );

    check(
        manager.load_directory(argv[1], "{}"),
        "Backpressure fixture loaded"
    );

    check(
        manager.plugin_count() == 1 &&
        manager.tool_count() == 1,
        "Exactly one plugin and one tool registered"
    );

    std::string result;
    std::string error;

    const bool blocker_executed = manager.execute_tool(
        "nakama_test_backpressure",
        "{\"mode\":\"blocker\"}",
        result,
        error
    );

    check(
        blocker_executed,
        blocker_executed
            ? "Initial blocker message published"
            : error.c_str()
    );

    bool entered = false;

    {
        std::unique_lock<std::mutex> lock(mutex);

        entered = condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] {
                return callback_entered;
            }
        );
    }

    check(
        entered,
        "Worker confirmed entry into blocked callback"
    );

    bool burst_executed = false;
    std::string burst_result;

    if (entered) {
        burst_executed = manager.execute_tool(
            "nakama_test_backpressure",
            "{\"mode\":\"burst\"}",
            burst_result,
            error
        );
    }

    check(
        burst_executed,
        burst_executed
            ? "Queue saturation tool executed"
            : error.c_str()
    );

    check(
        burst_result ==
            "{\"accepted\":1024,\"rejected\":1,\"status\":6}",
        "Exactly 1024 queued messages accepted; next rejected"
    );

    {
        std::lock_guard<std::mutex> lock(mutex);
        release_callback = true;
    }

    condition.notify_all();

    bool all_received = false;

    {
        std::unique_lock<std::mutex> lock(mutex);

        all_received = condition.wait_for(
            lock,
            std::chrono::seconds(10),
            [&] {
                return received >= 1025;
            }
        );
    }

    check(
        all_received,
        "All accepted messages delivered after callback release"
    );

    {
        std::lock_guard<std::mutex> lock(mutex);

        check(
            received == 1025,
            "One in-flight plus 1024 queued callbacks received"
        );

        check(
            invalid_messages == 0,
            "All message identities preserved"
        );
    }

    manager.clear_authenticated_message_handler();
    manager.shutdown();

    check(
        manager.plugin_count() == 0 &&
        manager.tool_count() == 0,
        "Shutdown completed and registries are empty"
    );

    std::cout << "\nFailures: " << failures << '\n';

    if (failures == 0) {
        std::cout << "ASYNC V2 BACKPRESSURE TEST: PASS\n";
        return 0;
    }

    std::cout << "ASYNC V2 BACKPRESSURE TEST: FAIL\n";
    return 1;
}
