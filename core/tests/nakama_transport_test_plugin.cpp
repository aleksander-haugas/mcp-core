
#include "mcp/plugin_abi.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>

namespace {

constexpr const char* PLUGIN_ID = "nakama";
constexpr const char* PRINCIPAL = "nakama-user-42";

struct PluginContext {
    const mcp_async_messages_v2* async = nullptr;
    void* host_context = nullptr;
    void* subscription = nullptr;

    std::mutex mutex;
    std::condition_variable received_cv;
    std::string received;
    bool has_received = false;
};

const char inbound_message[] =
    "{\"protocol\":\"agent\","
    "\"version\":1,"
    "\"type\":\"request\","
    "\"message_id\":\"fixture-inbound-001\","
    "\"source\":{\"agent_id\":\"remote-agent\","
    "\"instance_id\":\"instance-1\"},"
    "\"target\":{\"agent_id\":\"core-agent\","
    "\"instance_id\":\"core-instance\"},"
    "\"method\":\"test.echo\","
    "\"payload\":{\"input\":42},"
    "\"timestamp\":\"2026-11-22T12:00:00Z\"}";

void receive_from_core(
    void* user_data,
    const uint8_t* data,
    size_t data_size
)
{
    auto* context = static_cast<PluginContext*>(user_data);

    if (context == nullptr ||
        (data == nullptr && data_size != 0)) {
        return;
    }

    try {
        std::string message;

        if (data_size != 0) {
            message.assign(
                reinterpret_cast<const char*>(data),
                data_size
            );
        }

        {
            std::lock_guard<std::mutex> lock(context->mutex);
            context->received = std::move(message);
            context->has_received = true;
        }

        context->received_cv.notify_all();
    } catch (...) {
        // No exceptions may cross the plugin ABI.
    }
}

mcp_plugin_status initialize(
    const mcp_host_api* host,
    const char*,
    void** plugin_context
)
{
    if (host == nullptr ||
        plugin_context == nullptr ||
        host->get_extension == nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    *plugin_context = nullptr;

    try {
        const void* extension = nullptr;

        const mcp_plugin_status status = host->get_extension(
            host->host_context,
            MCP_PLUGIN_EXT_ASYNC_MESSAGES,
            MCP_PLUGIN_ASYNC_MESSAGES_VERSION_2,
            &extension
        );

        if (status != MCP_PLUGIN_OK || extension == nullptr) {
            return MCP_PLUGIN_NOT_SUPPORTED;
        }

        const auto* async =
            static_cast<const mcp_async_messages_v2*>(extension);

        if (async->version != MCP_PLUGIN_ASYNC_MESSAGES_VERSION_2 ||
            async->struct_size < sizeof(mcp_async_messages_v2) ||
            async->register_receiver == nullptr ||
            async->unregister_receiver == nullptr ||
            async->publish_authenticated == nullptr) {
            return MCP_PLUGIN_NOT_SUPPORTED;
        }

        auto* context = new PluginContext();
        context->async = async;
        context->host_context = host->host_context;

        const mcp_plugin_status register_status =
            async->register_receiver(
                host->host_context,
                receive_from_core,
                context,
                &context->subscription
            );

        if (register_status != MCP_PLUGIN_OK ||
            context->subscription == nullptr) {
            delete context;
            return MCP_PLUGIN_ERROR;
        }

        *plugin_context = context;
        return MCP_PLUGIN_OK;
    } catch (...) {
        return MCP_PLUGIN_ERROR;
    }
}

void shutdown(void* plugin_context)
{
    auto* context = static_cast<PluginContext*>(plugin_context);

    if (context == nullptr) {
        return;
    }

    if (context->async != nullptr &&
        context->subscription != nullptr) {
        context->async->unregister_receiver(
            context->host_context,
            context->subscription
        );
    }

    delete context;
}

const char* const publish_capabilities[] = {
    "nakama.test.publish"
};

const char* const receive_capabilities[] = {
    "nakama.test.receive"
};

const char TOOL_SCHEMA[] =
    "{\"type\":\"object\",\"properties\":{},"
    "\"additionalProperties\":false}";

const mcp_tool_definition tools[] = {
    {
        "nakama_test_publish",
        "Publish an authenticated inbound Agent Protocol message.",
        TOOL_SCHEMA,
        publish_capabilities,
        sizeof(publish_capabilities) / sizeof(publish_capabilities[0])
    },
    {
        "nakama_test_get_received",
        "Retrieve a message delivered from Core to this plugin.",
        TOOL_SCHEMA,
        receive_capabilities,
        sizeof(receive_capabilities) / sizeof(receive_capabilities[0])
    }
};

mcp_plugin_status list_tools(
    void*,
    const mcp_tool_definition** output,
    size_t* count
)
{
    if (output == nullptr || count == nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    *output = tools;
    *count = sizeof(tools) / sizeof(tools[0]);

    return MCP_PLUGIN_OK;
}

mcp_plugin_status write_result(
    const std::string& result,
    char* result_buffer,
    size_t* result_size
)
{
    if (result_size == nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    const size_t required = result.size() + 1;

    if (result_buffer == nullptr) {
        *result_size = required;
        return MCP_PLUGIN_BUFFER_TOO_SMALL;
    }

    if (*result_size < required) {
        *result_size = required;
        return MCP_PLUGIN_BUFFER_TOO_SMALL;
    }

    std::memcpy(result_buffer, result.c_str(), required);
    *result_size = required;

    return MCP_PLUGIN_OK;
}

mcp_plugin_status execute_tool(
    void* plugin_context,
    const char* tool_name,
    const char*,
    char* result_buffer,
    size_t* result_size
)
{
    auto* context = static_cast<PluginContext*>(plugin_context);

    if (context == nullptr ||
        tool_name == nullptr ||
        result_size == nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    try {
        const std::string name(tool_name);

        if (name == "nakama_test_publish") {
            if (result_buffer == nullptr) {
                return write_result(
                    "{\"published\":true}",
                    nullptr,
                    result_size
                );
            }

            mcp_authenticated_message_context metadata{};
            metadata.struct_size = sizeof(metadata);
            metadata.version =
                MCP_PLUGIN_AUTHENTICATED_MESSAGE_CONTEXT_VERSION_1;
            metadata.transport_id = "nakama";
            metadata.authenticated_principal = PRINCIPAL;

            const auto* bytes =
                reinterpret_cast<const uint8_t*>(inbound_message);

            const mcp_plugin_status status =
                context->async->publish_authenticated(
                    context->host_context,
                    &metadata,
                    bytes,
                    sizeof(inbound_message) - 1
                );

            if (status != MCP_PLUGIN_OK) {
                return status;
            }

            return write_result(
                "{\"published\":true}",
                result_buffer,
                result_size
            );
        }

        if (name == "nakama_test_get_received") {
            std::unique_lock<std::mutex> lock(context->mutex);

            const bool available = context->received_cv.wait_for(
                lock,
                std::chrono::seconds(5),
                [context] {
                    return context->has_received;
                }
            );

            if (!available) {
                return MCP_PLUGIN_ERROR;
            }

            const std::string message = context->received;

            if (result_buffer == nullptr) {
                return write_result(
                    message,
                    nullptr,
                    result_size
                );
            }

            const mcp_plugin_status status = write_result(
                message,
                result_buffer,
                result_size
            );

            if (status == MCP_PLUGIN_OK) {
                context->received.clear();
                context->has_received = false;
            }

            return status;
        }

        return MCP_PLUGIN_NOT_FOUND;
    } catch (...) {
        return MCP_PLUGIN_ERROR;
    }
}

const mcp_plugin_descriptor descriptor = {
    MCP_PLUGIN_ABI_VERSION,
    sizeof(mcp_plugin_descriptor),
    PLUGIN_ID,
    "1.0.0",
    "MCP Core Nakama AgentTransport integration test fixture",
    initialize,
    shutdown,
    list_tools,
    execute_tool
};

} // namespace

extern "C" MCP_PLUGIN_EXPORT
const mcp_plugin_descriptor* mcp_plugin_query(void)
{
    return &descriptor;
}
