#include "mcp/plugin_abi.h"

#include <cstdint>
#include <cstring>
#include <new>
#include <string>

namespace {

constexpr int QUEUE_CAPACITY = 1024;
constexpr int BURST_ATTEMPTS = QUEUE_CAPACITY + 1;

struct TestContext {
    void* host_context = nullptr;
    const mcp_async_messages_v2* api = nullptr;
};

static const char TOOL_SCHEMA[] =
    "{\"type\":\"object\",\"properties\":{},"
    "\"additionalProperties\":false}";

static const char* const CAPABILITIES[] = {
    "nakama.test.backpressure"
};

static const mcp_tool_definition TOOLS[] = {
    {
        "nakama_test_backpressure",
        "Tests async message queue backpressure",
        TOOL_SCHEMA,
        CAPABILITIES,
        1
    }
};

mcp_plugin_status initialize(
    const mcp_host_api* host,
    const char*,
    void** plugin_context
)
{
    if (host == nullptr ||
        plugin_context == nullptr ||
        host->get_extension == nullptr ||
        host->abi_version != MCP_PLUGIN_ABI_VERSION) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    auto* context = new (std::nothrow) TestContext();

    if (context == nullptr) {
        return MCP_PLUGIN_ERROR;
    }

    context->host_context = host->host_context;

    const void* extension = nullptr;

    const auto status = host->get_extension(
        host->host_context,
        MCP_PLUGIN_EXT_ASYNC_MESSAGES,
        MCP_PLUGIN_ASYNC_MESSAGES_VERSION_2,
        &extension
    );

    if (status != MCP_PLUGIN_OK || extension == nullptr) {
        delete context;
        return MCP_PLUGIN_ERROR;
    }

    context->api =
        static_cast<const mcp_async_messages_v2*>(extension);

    if (context->api->version !=
            MCP_PLUGIN_ASYNC_MESSAGES_VERSION_2 ||
        context->api->struct_size != sizeof(mcp_async_messages_v2) ||
        context->api->publish_authenticated == nullptr) {
        delete context;
        return MCP_PLUGIN_ERROR;
    }

    *plugin_context = context;
    return MCP_PLUGIN_OK;
}

void shutdown(void* plugin_context)
{
    delete static_cast<TestContext*>(plugin_context);
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
    *tool_count = 1;
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

    if (std::strcmp(
            tool_name,
            "nakama_test_backpressure") != 0) {
        return MCP_PLUGIN_NOT_FOUND;
    }

    const bool blocker =
        std::strcmp(arguments_json, "{\"mode\":\"blocker\"}") == 0;

    const bool burst =
        std::strcmp(arguments_json, "{\"mode\":\"burst\"}") == 0;

    if (!blocker && !burst) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    static const char BLOCKER_RESULT[] =
        "{\"accepted\":1}";

    static const char BURST_RESULT[] =
        "{\"accepted\":1024,\"rejected\":1,\"status\":6}";

    const char* result = blocker
        ? BLOCKER_RESULT
        : BURST_RESULT;

    const size_t required_size = std::strlen(result) + 1;

    if (result_buffer == nullptr ||
        *result_size < required_size) {
        *result_size = required_size;
        return MCP_PLUGIN_BUFFER_TOO_SMALL;
    }

    auto* context = static_cast<TestContext*>(plugin_context);

    static const char TRANSPORT[] = "nakama";
    static const char PRINCIPAL[] = "backpressure-test-user";

    mcp_authenticated_message_context message_context = {};
    message_context.struct_size = sizeof(message_context);
    message_context.version =
        MCP_PLUGIN_AUTHENTICATED_MESSAGE_CONTEXT_VERSION_1;
    message_context.transport_id = TRANSPORT;
    message_context.authenticated_principal = PRINCIPAL;

    if (blocker) {
        static const std::uint8_t payload[] = {
            'b', 'l', 'o', 'c', 'k', 'e', 'r'
        };

        const auto status = context->api->publish_authenticated(
            context->host_context,
            &message_context,
            payload,
            sizeof(payload)
        );

        if (status != MCP_PLUGIN_OK) {
            return status;
        }
    } else {
        int accepted = 0;
        int rejected = 0;
        mcp_plugin_status rejection_status = MCP_PLUGIN_OK;

        for (int i = 0; i < BURST_ATTEMPTS; ++i) {
            const std::string payload =
                "queued:" + std::to_string(i);

            const auto status =
                context->api->publish_authenticated(
                    context->host_context,
                    &message_context,
                    reinterpret_cast<const std::uint8_t*>(
                        payload.data()
                    ),
                    payload.size()
                );

            if (status == MCP_PLUGIN_OK) {
                ++accepted;
            } else if (status == MCP_PLUGIN_NOT_SUPPORTED) {
                ++rejected;
                rejection_status = status;
            } else {
                return status;
            }
        }

        if (accepted != QUEUE_CAPACITY ||
            rejected != 1 ||
            rejection_status != MCP_PLUGIN_NOT_SUPPORTED) {
            return MCP_PLUGIN_ERROR;
        }
    }

    std::memcpy(result_buffer, result, required_size);
    *result_size = required_size;

    return MCP_PLUGIN_OK;
}

const mcp_plugin_descriptor DESCRIPTOR = {
    MCP_PLUGIN_ABI_VERSION,
    sizeof(mcp_plugin_descriptor),
    "nakama",
    "0.1.0",
    "MCP Core async ABI v2 backpressure test plugin",
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
