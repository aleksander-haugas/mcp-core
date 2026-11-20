#include "mcp/plugin_manager.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

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
    int callback_count = 0;
    int invalid_messages = 0;

    mcp::PluginManager manager;

    manager.set_authenticated_message_handler(
        [&](const mcp::AuthenticatedPluginMessage& message) {
            std::unique_lock<std::mutex> lock(mutex);

            ++callback_count;

            if (message.plugin_id != "nakama" ||
                message.transport_id != "nakama" ||
                message.authenticated_principal !=
                    "concurrency-test-user") {
                ++invalid_messages;
            }

            if (callback_count == 1) {
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
        "Concurrency fixture loaded"
    );

    check(
        manager.plugin_count() == 1 &&
        manager.tool_count() == 1,
        "Exactly one plugin and one tool registered"
    );

    std::string result;
    std::string error;

    const bool executed = manager.execute_tool(
        "nakama_test_concurrency",
        "{}",
        result,
        error
    );

    check(
        executed,
        executed
            ? "Concurrent message batch published"
            : error.c_str()
    );

    check(
        result == "{\"published\":800}",
        "Plugin confirmed 800 successful publications"
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
        "Callback entered and is blocked"
    );

    if (!entered) {
        manager.shutdown();

        std::cout << "\nFailures: " << failures << '\n';
        std::cout << "ASYNC V2 SHUTDOWN DRAIN TEST: FAIL\n";
        return 1;
    }

    std::atomic<bool> shutdown_started{false};

    auto shutdown_future = std::async(
        std::launch::async,
        [&] {
            shutdown_started.store(true);
            manager.shutdown();
        }
    );

    while (!shutdown_started.load()) {
        std::this_thread::yield();
    }

    const auto early_result = shutdown_future.wait_for(
        std::chrono::milliseconds(300)
    );

    check(
        early_result == std::future_status::timeout,
        "Shutdown waits while callback is active"
    );

    {
        std::lock_guard<std::mutex> lock(mutex);
        release_callback = true;
    }

    condition.notify_all();

    const auto completion = shutdown_future.wait_for(
        std::chrono::seconds(5)
    );

    check(
        completion == std::future_status::ready,
        "Shutdown completes after callback is released"
    );

    if (completion == std::future_status::ready) {
        shutdown_future.get();
    } else {
        std::cerr << "[FAIL] Shutdown timed out\n";
        ++failures;

        {
            std::lock_guard<std::mutex> lock(mutex);
            release_callback = true;
        }

        condition.notify_all();
        shutdown_future.wait();
        shutdown_future.get();
    }

    int final_callback_count = 0;
    int final_invalid_messages = 0;

    {
        std::lock_guard<std::mutex> lock(mutex);
        final_callback_count = callback_count;
        final_invalid_messages = invalid_messages;
    }

    check(
        final_callback_count == 1,
        "No queued callbacks execute after shutdown"
    );

    check(
        final_invalid_messages == 0,
        "Authenticated identity preserved during shutdown"
    );

    check(
        manager.plugin_count() == 0 &&
        manager.tool_count() == 0,
        "Plugin and tool registries empty after shutdown"
    );

    std::cout << "\nCallbacks observed: "
              << final_callback_count << '\n';
    std::cout << "Failures: " << failures << '\n';

    if (failures == 0) {
        std::cout << "ASYNC V2 SHUTDOWN DRAIN TEST: PASS\n";
        return 0;
    }

    std::cout << "ASYNC V2 SHUTDOWN DRAIN TEST: FAIL\n";
    return 1;
}
