#include "mcp/plugin_abi.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int PRODUCERS = 8;
constexpr int MESSAGES_PER_PRODUCER = 100;

struct TestContext {
    void* host_context = nullptr;
    const mcp_async_messages_v2* api = nullptr;
};

static const char TOOL_SCHEMA[] =
    "{\"type\":\"object\",\"properties\":{},"
    "\"additionalProperties\":false}";

static const char* const CAPABILITIES[] = {
    "nakama.test.concurrency"
};

static const mcp_tool_definition TOOLS[] = {
    {
        "nakama_test_concurrency",
        "Publishes messages concurrently through async ABI v2",
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

    const mcp_plugin_status status = host->get_extension(
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
    *tool_count = sizeof(TOOLS) / sizeof(TOOLS[0]);
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
    if (plugin_context == nullptr ||
        tool_name == nullptr ||
        result_size == nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    if (std::strcmp(
            tool_name,
            "nakama_test_concurrency") != 0) {
        return MCP_PLUGIN_NOT_FOUND;
    }

    static const char RESULT[] = "{\"published\":800}";

    if (result_buffer == nullptr ||
        *result_size < sizeof(RESULT)) {
        *result_size = sizeof(RESULT);
        return MCP_PLUGIN_BUFFER_TOO_SMALL;
    }

    auto* context = static_cast<TestContext*>(plugin_context);

    static const char TRANSPORT[] = "nakama";
    static const char PRINCIPAL[] = "concurrency-test-user";

    mcp_authenticated_message_context message_context = {};
    message_context.struct_size = sizeof(message_context);
    message_context.version =
        MCP_PLUGIN_AUTHENTICATED_MESSAGE_CONTEXT_VERSION_1;
    message_context.transport_id = TRANSPORT;
    message_context.authenticated_principal = PRINCIPAL;

    std::atomic<int> successful_publications{0};
    std::atomic<int> failed_publications{0};

    std::vector<std::thread> producers;
    producers.reserve(PRODUCERS);

    try {
        for (int producer = 0; producer < PRODUCERS; ++producer) {
            producers.emplace_back([&, producer] {
                for (int sequence = 0;
                     sequence < MESSAGES_PER_PRODUCER;
                     ++sequence) {

                    const std::string payload =
                        std::to_string(producer) + ":" +
                        std::to_string(sequence);

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
                        successful_publications.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );
                    } else {
                        failed_publications.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );
                    }
                }
            });
        }
    } catch (...) {
        for (auto& producer : producers) {
            if (producer.joinable()) {
                producer.join();
            }
        }
        return MCP_PLUGIN_ERROR;
    }

    for (auto& producer : producers) {
        producer.join();
    }

    if (successful_publications.load() !=
            PRODUCERS * MESSAGES_PER_PRODUCER ||
        failed_publications.load() != 0) {
        return MCP_PLUGIN_ERROR;
    }

    std::memcpy(result_buffer, RESULT, sizeof(RESULT));
    *result_size = sizeof(RESULT);

    return MCP_PLUGIN_OK;
}

const mcp_plugin_descriptor DESCRIPTOR = {
    MCP_PLUGIN_ABI_VERSION,
    sizeof(mcp_plugin_descriptor),
    "nakama",
    "0.1.0",
    "MCP Core async ABI v2 concurrency test plugin",
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
