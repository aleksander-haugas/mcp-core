#include "mcp/plugin_manager.hpp"

#include <algorithm>
#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    int checks = 0;
    int failures = 0;

    auto check = [&](bool condition, const char* description) {
        ++checks;

        std::cout << (condition ? "[PASS] " : "[FAIL] ")
                  << description << '\n';

        if (!condition) {
            ++failures;
        }
    };

    if (argc != 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <isolated-plugin-directory>\n";
        return 2;
    }

    mcp::PluginManager manager;

    const bool loaded = manager.load_directory(argv[1], "{}");

    check(loaded, "Plugin directory scanned successfully");
    check(manager.plugin_count() == 1,
          "Exactly one plugin loaded");

    check(manager.tool_count() == 3,
          "Exactly three Nakama tools registered");

    const auto& tools = manager.tools();

    const char* expected_names[] = {
        "nakama_status",
        "nakama_ping",
        "nakama_disconnect"
    };

    bool definitions_valid = tools.size() == 3;

    if (definitions_valid) {
        for (const char* expected : expected_names) {
            const auto it = std::find_if(
                tools.begin(),
                tools.end(),
                [&](const mcp::RegisteredTool& tool) {
                    return tool.name == expected &&
                           tool.plugin_id == "nakama";
                }
            );

            if (it == tools.end() ||
                it->input_schema_json.empty()) {
                definitions_valid = false;
                break;
            }
        }
    }

    check(definitions_valid,
          "Tool names, plugin IDs and schemas registered correctly");

    auto has_capability = [&](const char* tool_name,
                              const char* capability) {
        const auto it = std::find_if(
            tools.begin(),
            tools.end(),
            [&](const mcp::RegisteredTool& tool) {
                return tool.name == tool_name;
            }
        );

        return it != tools.end() &&
            std::find(
                it->required_capabilities.begin(),
                it->required_capabilities.end(),
                capability
            ) != it->required_capabilities.end();
    };

    check(has_capability("nakama_status", "nakama.read"),
          "nakama_status requires nakama.read");

    check(has_capability("nakama_ping", "nakama.read"),
          "nakama_ping requires nakama.read");

    check(has_capability("nakama_disconnect", "nakama.control"),
          "nakama_disconnect requires nakama.control");

    std::string result;
    std::string error;

    const bool status_ok = manager.execute_tool(
        "nakama_status", "{}", result, error
    );

    check(status_ok, "nakama_status executes through PluginManager");

    check(
        status_ok &&
        result.find("\"configured\":false") != std::string::npos &&
        result.find("\"configuration_error\":true") !=
        std::string::npos &&
        result.find("\"configuration_state\":\"INVALID\"") !=
        std::string::npos,
        "Invalid system configuration is reported correctly"
    );

    result.clear();
    error.clear();

    const bool ping_ok = manager.execute_tool(
        "nakama_ping", "{}", result, error
    );

    check(!ping_ok,
          "nakama_ping is rejected before transport setup");

    check(!ping_ok &&
          error.find("NOT_SUPPORTED") != std::string::npos,
          "PluginManager propagates NOT_SUPPORTED");

    result.clear();
    error.clear();

    const bool disconnect_ok = manager.execute_tool(
        "nakama_disconnect", "{}", result, error
    );

    check(disconnect_ok,
          "nakama_disconnect executes through PluginManager");

    check(disconnect_ok &&
          result.find("\"disconnected\":true") != std::string::npos &&
          result.find("\"already_stopped\":true") != std::string::npos,
          "Disconnect reports the already-stopped state");

    result.clear();
    error.clear();

    check(!manager.execute_tool(
              "nakama_unknown", "{}", result, error),
          "Unknown tool is rejected");

    manager.shutdown();

    check(manager.plugin_count() == 0,
          "PluginManager plugin registry cleared after shutdown");

    check(manager.tool_count() == 0,
          "PluginManager tool registry cleared after shutdown");

    std::cout << "\nPluginManager integration checks: "
              << checks << ", failures: " << failures << '\n';

    return failures == 0 ? 0 : 1;
}
