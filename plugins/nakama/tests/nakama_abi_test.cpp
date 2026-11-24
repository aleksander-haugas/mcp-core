#include "mcp/plugin_abi.h"

#include <dlfcn.h>
#include <json-c/json.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* description)
{
    ++checks;

    std::printf(
        "[%s] %s\n",
        condition ? "PASS" : "FAIL",
        description
    );

    if (!condition) {
        ++failures;
    }
}

void host_log(
    void*,
    mcp_log_level,
    const char*,
    const char*
)
{
}

bool valid_json(const char* text)
{
    if (text == nullptr) {
        return false;
    }

    json_tokener* tokener = json_tokener_new();

    if (tokener == nullptr) {
        return false;
    }

    json_object* object = json_tokener_parse_ex(
        tokener,
        text,
        static_cast<int>(std::strlen(text))
    );

    const bool valid =
        json_tokener_get_error(tokener) == json_tokener_success &&
        object != nullptr;

    if (object != nullptr) {
        json_object_put(object);
    }

    json_tokener_free(tokener);
    return valid;
}

bool has_json_boolean(
    const char* text,
    const char* key,
    bool expected
)
{
    json_tokener* tokener = json_tokener_new();

    if (tokener == nullptr) {
        return false;
    }

    json_object* root = json_tokener_parse_ex(
        tokener,
        text,
        static_cast<int>(std::strlen(text))
    );

    bool result = false;
    json_object* value = nullptr;

    if (root != nullptr &&
        json_tokener_get_error(tokener) == json_tokener_success &&
        json_object_get_type(root) == json_type_object &&
        json_object_object_get_ex(root, key, &value) &&
        value != nullptr &&
        json_object_get_type(value) == json_type_boolean) {
        result = json_object_get_boolean(value) == expected;
    }

    if (root != nullptr) {
        json_object_put(root);
    }

    json_tokener_free(tokener);
    return result;
}

bool has_capability(
    const mcp_tool_definition& tool,
    const char* expected
)
{
    for (size_t i = 0; i < tool.required_capabilities_count; ++i) {
        if (tool.required_capabilities[i] != nullptr &&
            std::strcmp(tool.required_capabilities[i], expected) == 0) {
            return true;
        }
    }

    return false;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(
            stderr,
            "Usage: %s /path/to/libmcp_nakama.so\n",
            argv[0]
        );
        return 2;
    }

    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);

    check(library != nullptr, "Shared library loaded with dlopen");

    if (library == nullptr) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }

    dlerror();

    auto query = reinterpret_cast<mcp_plugin_query_fn>(
        dlsym(library, MCP_PLUGIN_ENTRY_SYMBOL)
    );

    const char* symbol_error = dlerror();

    check(
        query != nullptr && symbol_error == nullptr,
        "mcp_plugin_query symbol resolved"
    );

    if (query == nullptr || symbol_error != nullptr) {
        dlclose(library);
        return 1;
    }

    const mcp_plugin_descriptor* descriptor = query();

    check(descriptor != nullptr, "Descriptor returned");
    if (descriptor == nullptr) {
        dlclose(library);
        return 1;
    }

    check(
        descriptor->abi_version == MCP_PLUGIN_ABI_VERSION,
        "ABI version matches"
    );

    check(
        descriptor->struct_size >= sizeof(mcp_plugin_descriptor),
        "Descriptor structure size is valid"
    );

    check(
        descriptor->plugin_id != nullptr &&
        std::strcmp(descriptor->plugin_id, "nakama") == 0,
        "Plugin ID is nakama"
    );

    check(
        descriptor->plugin_version != nullptr &&
        std::strcmp(descriptor->plugin_version, "0.1.0") == 0,
        "Plugin version is 0.1.0"
    );

    check(
        descriptor->description != nullptr &&
        descriptor->description[0] != '\0',
        "Plugin description is present"
    );

    check(
        descriptor->initialize != nullptr &&
        descriptor->shutdown != nullptr &&
        descriptor->list_tools != nullptr &&
        descriptor->execute_tool != nullptr,
        "All mandatory callbacks are present"
    );

    if (descriptor->initialize == nullptr ||
        descriptor->shutdown == nullptr ||
        descriptor->list_tools == nullptr ||
        descriptor->execute_tool == nullptr) {
        dlclose(library);
        return 1;
    }

    mcp_host_api host = {};
    host.abi_version = MCP_PLUGIN_ABI_VERSION;
    host.struct_size = sizeof(host);
    host.log = host_log;

    void* context = nullptr;

    check(
        descriptor->initialize(&host, "{}", &context) == MCP_PLUGIN_OK,
        "Plugin initialization succeeds"
    );

    check(context != nullptr, "Plugin context is created");

    if (context == nullptr) {
        dlclose(library);
        return 1;
    }

    const mcp_tool_definition* tools = nullptr;
    size_t tool_count = 0;

    check(
        descriptor->list_tools(context, &tools, &tool_count) ==
            MCP_PLUGIN_OK,
        "Tool registration succeeds"
    );

    check(
        tools != nullptr && tool_count == 4,
        "Exactly four tools are registered"
    );

    const char* expected_names[] = {
        "nakama_connect",
        "nakama_status",
        "nakama_ping",
        "nakama_disconnect"
    };

    bool definitions_valid = tools != nullptr && tool_count == 4;

    if (definitions_valid) {
        for (size_t i = 0; i < tool_count; ++i) {
            if (tools[i].name == nullptr ||
                tools[i].description == nullptr ||
                tools[i].input_schema_json == nullptr ||
                std::strcmp(tools[i].name, expected_names[i]) != 0 ||
                !valid_json(tools[i].input_schema_json)) {
                definitions_valid = false;
            }
        }
    }

    check(
        definitions_valid,
        "Tool names, descriptions and JSON schemas are valid"
    );

    if (tools != nullptr && tool_count == 4) {
        check(
            has_capability(tools[0], "nakama.control"),
            "nakama_connect requires nakama.control"
        );

        check(
            has_capability(tools[1], "nakama.read"),
            "nakama_status requires nakama.read"
        );

        check(
            has_capability(tools[2], "nakama.read"),
            "nakama_ping requires nakama.read"
        );

        check(
            has_capability(tools[3], "nakama.control"),
            "nakama_disconnect requires nakama.control"
        );

        check(
            tools[0].required_capabilities_count == 1 &&
            tools[1].required_capabilities_count == 1 &&
            tools[2].required_capabilities_count == 1 &&
            tools[3].required_capabilities_count == 1,
            "Each tool declares exactly one capability"
        );
    }

    auto execute = descriptor->execute_tool;

    size_t required = 0;

    mcp_plugin_status status = execute(
        context,
        "nakama_status",
        "{}",
        nullptr,
        &required
    );

    check(
        status == MCP_PLUGIN_BUFFER_TOO_SMALL && required > 1,
        "Status supports first pass buffer sizing"
    );

    std::vector<char> status_buffer(required, '\0');
    size_t status_capacity = status_buffer.size();

    status = execute(
        context,
        "nakama_status",
        "{}",
        status_buffer.data(),
        &status_capacity
    );

    check(
        status == MCP_PLUGIN_OK,
        "nakama_status executes successfully"
    );

    check(
        status == MCP_PLUGIN_OK && valid_json(status_buffer.data()),
        "nakama_status returns valid JSON"
    );

    check(
        status == MCP_PLUGIN_OK &&
        std::strstr(
            status_buffer.data(),
            "\"transport_state\":\"STOPPED\""
        ) != nullptr &&
        has_json_boolean(status_buffer.data(), "connected", false),
        "Status reports stopped and disconnected transport"
    );

    required = 0;

    status = execute(
        context,
        "nakama_disconnect",
        "{}",
        nullptr,
        &required
    );

    check(
        status == MCP_PLUGIN_BUFFER_TOO_SMALL && required > 1,
        "Disconnect supports first pass buffer sizing"
    );

    std::vector<char> disconnect_buffer(required, '\0');
    size_t disconnect_capacity = disconnect_buffer.size();

    status = execute(
        context,
        "nakama_disconnect",
        "{}",
        disconnect_buffer.data(),
        &disconnect_capacity
    );

    check(
        status == MCP_PLUGIN_OK &&
        valid_json(disconnect_buffer.data()),
        "nakama_disconnect returns valid JSON"
    );

    check(
        status == MCP_PLUGIN_OK &&
        has_json_boolean(disconnect_buffer.data(), "disconnected", true) &&
        has_json_boolean(disconnect_buffer.data(), "was_connected", false),
        "Disconnect reports disconnected and previously stopped state"
    );

    size_t small_size = 1;
    char small_buffer[1] = {'X'};

    status = execute(
        context,
        "nakama_disconnect",
        "{}",
        small_buffer,
        &small_size
    );

    check(
        status == MCP_PLUGIN_BUFFER_TOO_SMALL && small_size > 1,
        "Insufficient output buffer reports required size"
    );

    size_t ping_size = 0;

    status = execute(
        context,
        "nakama_ping",
        "{}",
        nullptr,
        &ping_size
    );

    std::vector<char> ping_buffer(ping_size, '\0');
    size_t ping_capacity = ping_buffer.size();

    status = execute(
        context,
        "nakama_ping",
        "{}",
        ping_buffer.data(),
        &ping_capacity
    );

    check(
        status == MCP_PLUGIN_OK && valid_json(ping_buffer.data()),
        "nakama_ping returns a valid disconnected response"
    );

    check(
        status == MCP_PLUGIN_OK &&
        has_json_boolean(ping_buffer.data(), "pong", false),
        "nakama_ping reports pong false when disconnected"
    );

    size_t invalid_size = 0;

    check(
        execute(
            context,
            "nakama_status",
            "{\"unexpected\":true}",
            nullptr,
            &invalid_size
        ) == MCP_PLUGIN_INVALID_ARGUMENT,
        "Unexpected tool arguments are rejected"
    );

    check(
        execute(
            context,
            "unknown_tool",
            "{}",
            nullptr,
            &invalid_size
        ) == MCP_PLUGIN_NOT_FOUND,
        "Unknown tool is rejected"
    );

    check(
        execute(
            context,
            "nakama_status",
            nullptr,
            nullptr,
            &invalid_size
        ) == MCP_PLUGIN_INVALID_ARGUMENT,
        "Null arguments are rejected"
    );

    check(
        execute(
            context,
            nullptr,
            "{}",
            nullptr,
            &invalid_size
        ) == MCP_PLUGIN_INVALID_ARGUMENT,
        "Null tool name is rejected"
    );

    check(
        execute(
            context,
            "nakama_status",
            "{}",
            nullptr,
            nullptr
        ) == MCP_PLUGIN_INVALID_ARGUMENT,
        "Null result size is rejected"
    );

    const mcp_plugin_status null_host_status =
        descriptor->initialize(nullptr, "{}", &context);

    check(
        null_host_status == MCP_PLUGIN_INVALID_ARGUMENT,
        "Null host API is rejected"
    );

    descriptor->shutdown(context);
    context = nullptr;

    check(true, "Plugin shutdown callback completed");

    const int close_result = dlclose(library);

    check(
        close_result == 0,
        "Shared library unloaded with dlclose"
    );

    std::printf(
        "\nABI checks: %d, failures: %d\n",
        checks,
        failures
    );

    return failures == 0 ? 0 : 1;
}
