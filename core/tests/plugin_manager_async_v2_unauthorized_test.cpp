#include "mcp/plugin_manager.hpp"

#include <chrono>
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
    int received_count = 0;

    mcp::PluginManager manager;

    manager.set_authenticated_message_handler(
        [&](const mcp::AuthenticatedPluginMessage&) {
            std::lock_guard<std::mutex> lock(mutex);
            ++received_count;
        }
    );

    check(
        manager.load_directory(argv[1], "{}"),
        "Unauthorized plugin directory loaded"
    );

    check(
        manager.plugin_count() == 1,
        "Exactly one unauthorized plugin loaded"
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
        executed ? "Unauthorized publication test executed"
                 : error.c_str()
    );

    check(
        result == "{\"published\":true}",
        "Plugin handled permission denial as expected"
    );

    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    {
        std::lock_guard<std::mutex> lock(mutex);

        check(
            received_count == 0,
            "Unauthorized plugin produced no Core deliveries"
        );
    }

    manager.clear_authenticated_message_handler();
    manager.shutdown();

    check(
        manager.plugin_count() == 0 &&
        manager.tool_count() == 0,
        "Unauthorized plugin shutdown completed"
    );

    std::cout << "\nFailures: " << failures << '\n';

    if (failures == 0) {
        std::cout << "UNAUTHORIZED ASYNC V2 TEST: PASS\n";
        return 0;
    }

    std::cout << "UNAUTHORIZED ASYNC V2 TEST: FAIL\n";
    return 1;
}
