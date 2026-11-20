#include "mcp/plugin_manager.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
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
            ? "Plugin published its concurrent message batch"
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
        "First callback entered and is now blocked"
    );

    if (!entered) {
        manager.clear_authenticated_message_handler();
        manager.shutdown();

        std::cout << "\nFailures: " << failures << '\n';
        std::cout << "ASYNC V2 HANDLER DRAIN TEST: FAIL\n";
        return 1;
    }

    std::atomic<bool> clear_started{false};

    auto clear_future = std::async(
        std::launch::async,
        [&] {
            clear_started.store(true);
            manager.clear_authenticated_message_handler();
        }
    );

    while (!clear_started.load()) {
        std::this_thread::yield();
    }

    const auto early_result = clear_future.wait_for(
        std::chrono::milliseconds(300)
    );

    check(
        early_result == std::future_status::timeout,
        "clear_authenticated_message_handler waits for active callback"
    );

    {
        std::lock_guard<std::mutex> lock(mutex);
        release_callback = true;
    }

    condition.notify_all();

    const auto completion = clear_future.wait_for(
        std::chrono::seconds(5)
    );

    check(
        completion == std::future_status::ready,
        "Handler clear completes after callback is released"
    );

    if (completion == std::future_status::ready) {
        clear_future.get();
    } else {
        std::cerr << "[FAIL] Handler clear timed out\n";
        ++failures;

        // Do not destroy the manager while its clear operation is active.
        {
            std::lock_guard<std::mutex> lock(mutex);
            release_callback = true;
        }

        condition.notify_all();
        clear_future.wait();
        clear_future.get();
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
        "No queued callbacks execute after handler clear"
    );

    check(
        final_invalid_messages == 0,
        "Callback identity remained valid"
    );

    manager.shutdown();

    check(
        manager.plugin_count() == 0 &&
        manager.tool_count() == 0,
        "Shutdown completed and registries are empty"
    );

    std::cout << "\nCallbacks observed: "
              << final_callback_count << '\n';
    std::cout << "Failures: " << failures << '\n';

    if (failures == 0) {
        std::cout << "ASYNC V2 HANDLER DRAIN TEST: PASS\n";
        return 0;
    }

    std::cout << "ASYNC V2 HANDLER DRAIN TEST: FAIL\n";
    return 1;
}
