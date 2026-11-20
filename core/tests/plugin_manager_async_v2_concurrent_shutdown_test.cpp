#include "mcp/plugin_manager.hpp"

#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <string>

using namespace std::chrono_literals;

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " /path/to/plugins\n";
        return 2;
    }

    int failures = 0;

    auto check = [&](bool ok, const char* message) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ")
                  << message << '\n';
        if (!ok) ++failures;
    };

    mcp::PluginManager manager;

    std::mutex mutex;
    std::condition_variable condition;

    bool callback_entered = false;
    bool release_callback = false;
    bool callback_shutdown_returned = false;
    int callback_count = 0;

    manager.set_authenticated_message_handler(
        [&](const mcp::AuthenticatedPluginMessage&) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                ++callback_count;
                callback_entered = true;
                condition.notify_all();

                condition.wait(lock, [&] {
                    return release_callback;
                });
            }

            // This runs on the async worker while both external
            // shutdown callers are waiting for this callback.
            manager.shutdown();

            {
                std::lock_guard<std::mutex> lock(mutex);
                callback_shutdown_returned = true;
            }
            condition.notify_all();
        }
    );

    check(
        manager.load_directory(argv[1], "{}"),
        "Plugin loaded"
    );

    check(
        manager.plugin_count() == 1 && manager.tool_count() == 1,
        "Plugin and tool registered"
    );

    std::string result;
    std::string error;

    check(
        manager.execute_tool(
            "nakama_test_publish", "{}", result, error
        ),
        "Publication tool executed"
    );

    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = condition.wait_for(lock, 5s, [&] {
            return callback_entered;
        });
    }

    check(entered, "Worker callback entered and blocked");

    if (!entered) {
        manager.shutdown();
        std::cout << "Failures: " << ++failures << '\n';
        return 1;
    }

    auto shutdown_a = std::async(std::launch::async, [&] {
        manager.shutdown();
    });

    auto shutdown_b = std::async(std::launch::async, [&] {
        manager.shutdown();
    });

    const auto wait_a = shutdown_a.wait_for(300ms);
    const auto wait_b = shutdown_b.wait_for(300ms);

    check(
        wait_a == std::future_status::timeout,
        "First external shutdown waits for active callback"
    );

    check(
        wait_b == std::future_status::timeout,
        "Second external shutdown waits for owner shutdown"
    );

    {
        std::lock_guard<std::mutex> lock(mutex);
        release_callback = true;
    }
    condition.notify_all();

    const auto callback_wait = [&] {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock, 5s, [&] {
            return callback_shutdown_returned;
        });
    }();

    check(
        callback_wait,
        "Worker callback safely requested deferred shutdown"
    );

    const auto result_a = shutdown_a.wait_for(5s);
    const auto result_b = shutdown_b.wait_for(5s);

    check(
        result_a == std::future_status::ready,
        "First external shutdown completed"
    );

    check(
        result_b == std::future_status::ready,
        "Second external shutdown completed"
    );

    // Ensure async tasks are joined before inspecting manager state.
    shutdown_a.get();
    shutdown_b.get();

    check(
        callback_count == 1,
        "Exactly one callback executed"
    );

    check(
        manager.plugin_count() == 0 && manager.tool_count() == 0,
        "Plugin and tool registries cleared"
    );

    manager.shutdown();

    std::cout << "\nFailures: " << failures << '\n';
    std::cout << (failures == 0
        ? "ASYNC V2 CONCURRENT SHUTDOWN TEST: PASS\n"
        : "ASYNC V2 CONCURRENT SHUTDOWN TEST: FAIL\n");

    return failures == 0 ? 0 : 1;
}
