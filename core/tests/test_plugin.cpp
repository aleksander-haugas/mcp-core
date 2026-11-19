#include "mcp/plugin_abi.h"

#include <cstring>

namespace {

struct TestPluginContext {
    const mcp_host_api* host;
};

static const char RESULT_JSON[] =
    "{\"message\":\"hello from test plugin\"}";

static const char* const TEST_CAPABILITIES[] = {
    "test.echo"
};

static const mcp_tool_definition TOOLS[] = {
    {
        "core_test_echo",
        "Test tool for MCP Core Plugin Manager",
        "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
        TEST_CAPABILITIES,
        sizeof(TEST_CAPABILITIES) /
        sizeof(TEST_CAPABILITIES[0])
    }
};

mcp_plugin_status initialize(
    const mcp_host_api* host,
    const char* configuration_json,
    void** plugin_context
)
{
    (void)configuration_json;

    if (host == nullptr ||
        plugin_context == nullptr ||
        host->abi_version != MCP_PLUGIN_ABI_VERSION) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    auto* context = new TestPluginContext{host};

    *plugin_context = context;

    if (host->log != nullptr) {
        host->log(
            host->host_context,
            MCP_LOG_INFO,
            "test",
            "Test plugin initialized"
        );
    }

    return MCP_PLUGIN_OK;
}

void shutdown(void* plugin_context)
{
    delete static_cast<TestPluginContext*>(plugin_context);
}

mcp_plugin_status list_tools(
    void* plugin_context,
    const mcp_tool_definition** tools,
    size_t* tool_count
)
{
    if (plugin_context == nullptr ||
        tools == nullptr ||
        tool_count == nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    *tools = TOOLS;
    *tool_count = sizeof(TOOLS) / sizeof(TOOLS[0]);

    return MCP_PLUGIN_OK;
}

mcp_plugin_status execute_tool(
    void* plugin_context,
    const char* tool_name,
    const char* arguments_json,
    char* result_buffer,
    size_t* result_size
)
{
    if (plugin_context == nullptr ||
        tool_name == nullptr ||
        arguments_json == nullptr ||
        result_size == nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    if (std::strcmp(tool_name, "core_test_echo") != 0) {
        return MCP_PLUGIN_NOT_FOUND;
    }

    const size_t required_size = sizeof(RESULT_JSON);

    if (result_buffer == nullptr ||
        *result_size < required_size) {
        *result_size = required_size;
        return MCP_PLUGIN_BUFFER_TOO_SMALL;
    }

    std::memcpy(
        result_buffer,
        RESULT_JSON,
        required_size
    );

    *result_size = required_size;

    return MCP_PLUGIN_OK;
}

const mcp_plugin_descriptor DESCRIPTOR = {
    MCP_PLUGIN_ABI_VERSION,
    sizeof(mcp_plugin_descriptor),
    "test",
    "1.0.0",
    "MCP Core Plugin Manager test plugin",
    initialize,
    shutdown,
    list_tools,
    execute_tool
};

} // namespace

extern "C" MCP_PLUGIN_EXPORT
const mcp_plugin_descriptor* mcp_plugin_query(void)
{
    return &DESCRIPTOR;
}
