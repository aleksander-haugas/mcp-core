#include "mcp/plugin_manager.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " /path/to/plugins\n";
        return 2;
    }

    int failures = 0;
    auto check = [&](bool ok, const char* text) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << text << '\n';
        if (!ok) ++failures;
    };

    mcp::PluginManager manager;
    std::mutex mutex;
    std::condition_variable condition;
    bool callback_finished = false;

    manager.set_authenticated_message_handler(
        [&](const mcp::AuthenticatedPluginMessage&) {
            manager.shutdown();

            {
                std::lock_guard<std::mutex> lock(mutex);
                callback_finished = true;
            }
            condition.notify_one();
        }
    );

    check(manager.load_directory(argv[1], "{}"), "Plugin loaded");

    std::string result;
    std::string error;

    check(
        manager.execute_tool("nakama_test_publish", "{}", result, error),
        "Publication tool executed"
    );

    bool arrived = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        arrived = condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return callback_finished; }
        );
    }

    check(arrived, "Worker callback requested deferred shutdown");

    if (arrived) {
        result.clear();
        error.clear();

        check(
            !manager.execute_tool(
                "nakama_test_publish", "{}", result, error
            ),
            "Tool execution rejected after shutdown request"
        );

        check(
            !manager.load_directory(argv[1], "{}"),
            "Plugin loading rejected after shutdown request"
        );

        check(
            manager.plugin_count() == 1,
            "Plugin retained until owner completes shutdown"
        );
    }

    manager.shutdown();

    check(
        manager.plugin_count() == 0 && manager.tool_count() == 0,
        "Owner completed shutdown and cleared registries"
    );

    std::cout << "\nFailures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
