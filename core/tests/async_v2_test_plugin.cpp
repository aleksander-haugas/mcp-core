#include "mcp/plugin_abi.h"

#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#ifndef TEST_PLUGIN_ID
#define TEST_PLUGIN_ID "nakama"
#endif

namespace {

constexpr uint64_t UNKNOWN_EXTENSION = 0x4D4350FFULL;

struct TestContext {
    void* host_context = nullptr;
    const mcp_async_messages_v1* v1 = nullptr;
    const mcp_async_messages_v2* v2 = nullptr;
};

static const char TOOL_SCHEMA[] =
    "{\"type\":\"object\",\"properties\":{},"
    "\"additionalProperties\":false}";

static const char* const TEST_CAPABILITIES[] = {
    "nakama.test.publish"
};

static const mcp_tool_definition TOOLS[] = {
    {
        "nakama_test_publish",
        "Publishes an authenticated test message",
        TOOL_SCHEMA,
        TEST_CAPABILITIES,
        sizeof(TEST_CAPABILITIES) / sizeof(TEST_CAPABILITIES[0])
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

    if (host->get_extension(
            host->host_context,
            MCP_PLUGIN_EXT_ASYNC_MESSAGES,
            MCP_PLUGIN_ASYNC_MESSAGES_VERSION_1,
            &extension) != MCP_PLUGIN_OK ||
        extension == nullptr) {
        delete context;
        return MCP_PLUGIN_ERROR;
    }

    context->v1 =
        static_cast<const mcp_async_messages_v1*>(extension);

    if (context->v1->version !=
            MCP_PLUGIN_ASYNC_MESSAGES_VERSION_1 ||
        context->v1->struct_size != sizeof(mcp_async_messages_v1) ||
        context->v1->register_receiver == nullptr ||
        context->v1->unregister_receiver == nullptr ||
        context->v1->publish == nullptr) {
        delete context;
        return MCP_PLUGIN_ERROR;
    }

    extension = nullptr;

    if (host->get_extension(
            host->host_context,
            MCP_PLUGIN_EXT_ASYNC_MESSAGES,
            MCP_PLUGIN_ASYNC_MESSAGES_VERSION_2,
            &extension) != MCP_PLUGIN_OK ||
        extension == nullptr) {
        delete context;
        return MCP_PLUGIN_ERROR;
    }

    context->v2 =
        static_cast<const mcp_async_messages_v2*>(extension);

    if (context->v2->version !=
            MCP_PLUGIN_ASYNC_MESSAGES_VERSION_2 ||
        context->v2->struct_size != sizeof(mcp_async_messages_v2) ||
        context->v2->register_receiver != context->v1->register_receiver ||
        context->v2->unregister_receiver != context->v1->unregister_receiver ||
        context->v2->publish != context->v1->publish ||
        context->v2->publish_authenticated == nullptr) {
        delete context;
        return MCP_PLUGIN_ERROR;
    }

    extension = reinterpret_cast<const void*>(1);

    if (host->get_extension(
            host->host_context,
            UNKNOWN_EXTENSION,
            MCP_PLUGIN_ASYNC_MESSAGES_VERSION_1,
            &extension) != MCP_PLUGIN_NOT_SUPPORTED ||
        extension != nullptr) {
        delete context;
        return MCP_PLUGIN_ERROR;
    }

    extension = reinterpret_cast<const void*>(1);

    if (host->get_extension(
            host->host_context,
            MCP_PLUGIN_EXT_ASYNC_MESSAGES,
            MCP_PLUGIN_ASYNC_MESSAGES_VERSION_2 + 1,
            &extension) != MCP_PLUGIN_NOT_SUPPORTED ||
        extension != nullptr) {
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

    auto* context = static_cast<TestContext*>(plugin_context);

    if (std::strcmp(tool_name, "nakama_test_publish") != 0) {
        return MCP_PLUGIN_NOT_FOUND;
    }

    static const char RESULT[] = "{\"published\":true}";

    if (result_buffer == nullptr ||
        *result_size < sizeof(RESULT)) {
        *result_size = sizeof(RESULT);
        return MCP_PLUGIN_BUFFER_TOO_SMALL;
    }

    static const char TRANSPORT[] = "nakama";
    static const char PRINCIPAL[] = "nakama-user-42";

    mcp_authenticated_message_context message_context = {};
    message_context.struct_size = sizeof(message_context);
    message_context.version =
        MCP_PLUGIN_AUTHENTICATED_MESSAGE_CONTEXT_VERSION_1;
    message_context.transport_id = TRANSPORT;
    message_context.authenticated_principal = PRINCIPAL;

    auto expect_status = [&](mcp_plugin_status actual,
                             mcp_plugin_status expected) {
        return actual == expected;
    };

    // 1. Null context.
    if (!expect_status(
            context->v2->publish_authenticated(
                context->host_context,
                nullptr,
                nullptr,
                0
            ),
            MCP_PLUGIN_INVALID_ARGUMENT)) {
        return MCP_PLUGIN_ERROR;
    }

    // 2. Invalid context size.
    auto invalid_context = message_context;
    invalid_context.struct_size = sizeof(message_context) - 1;

    if (!expect_status(
            context->v2->publish_authenticated(
                context->host_context,
                &invalid_context,
                nullptr,
                0
            ),
            MCP_PLUGIN_INVALID_ARGUMENT)) {
        return MCP_PLUGIN_ERROR;
    }

    // 3. Invalid context version.
    invalid_context = message_context;
    invalid_context.version =
        MCP_PLUGIN_AUTHENTICATED_MESSAGE_CONTEXT_VERSION_1 + 1;

    if (!expect_status(
            context->v2->publish_authenticated(
                context->host_context,
                &invalid_context,
                nullptr,
                0
            ),
            MCP_PLUGIN_INVALID_ARGUMENT)) {
        return MCP_PLUGIN_ERROR;
    }

    // 4. Null transport.
    invalid_context = message_context;
    invalid_context.transport_id = nullptr;

    if (!expect_status(
            context->v2->publish_authenticated(
                context->host_context,
                &invalid_context,
                nullptr,
                0
            ),
            MCP_PLUGIN_INVALID_ARGUMENT)) {
        return MCP_PLUGIN_ERROR;
    }

    // 5. Null principal.
    invalid_context = message_context;
    invalid_context.authenticated_principal = nullptr;

    if (!expect_status(
            context->v2->publish_authenticated(
                context->host_context,
                &invalid_context,
                nullptr,
                0
            ),
            MCP_PLUGIN_INVALID_ARGUMENT)) {
        return MCP_PLUGIN_ERROR;
    }

    // 6. Empty transport.
    invalid_context = message_context;
    invalid_context.transport_id = "";

    if (!expect_status(
            context->v2->publish_authenticated(
                context->host_context,
                &invalid_context,
                nullptr,
                0
            ),
            MCP_PLUGIN_INVALID_ARGUMENT)) {
        return MCP_PLUGIN_ERROR;
    }

    // 7. Empty principal.
    invalid_context = message_context;
    invalid_context.authenticated_principal = "";

    if (!expect_status(
            context->v2->publish_authenticated(
                context->host_context,
                &invalid_context,
                nullptr,
                0
            ),
            MCP_PLUGIN_INVALID_ARGUMENT)) {
        return MCP_PLUGIN_ERROR;
    }

    // 8. Transport longer than 64 bytes.
    const std::string long_transport(65, 'T');
    invalid_context = message_context;
    invalid_context.transport_id = long_transport.c_str();

    if (!expect_status(
            context->v2->publish_authenticated(
                context->host_context,
                &invalid_context,
                nullptr,
                0
            ),
            MCP_PLUGIN_INVALID_ARGUMENT)) {
        return MCP_PLUGIN_ERROR;
    }

    // 9. Principal longer than 256 bytes.
    const std::string long_principal(257, 'P');
    invalid_context = message_context;
    invalid_context.authenticated_principal =
        long_principal.c_str();

    if (!expect_status(
            context->v2->publish_authenticated(
                context->host_context,
                &invalid_context,
                nullptr,
                0
            ),
            MCP_PLUGIN_INVALID_ARGUMENT)) {
        return MCP_PLUGIN_ERROR;
    }

    // 10. Null payload with nonzero size.
    if (!expect_status(
            context->v2->publish_authenticated(
                context->host_context,
                &message_context,
                nullptr,
                1
            ),
            MCP_PLUGIN_INVALID_ARGUMENT)) {
        return MCP_PLUGIN_ERROR;
    }

    // 11. Payload exceeding 1 MiB.
    const std::vector<std::uint8_t> oversized_payload(
        1024 * 1024 + 1,
        0x41
    );

    if (!expect_status(
            context->v2->publish_authenticated(
                context->host_context,
                &message_context,
                oversized_payload.data(),
                oversized_payload.size()
            ),
            MCP_PLUGIN_INVALID_ARGUMENT)) {
        return MCP_PLUGIN_ERROR;
    }

    // 12. Unauthorized transport identity.
    invalid_context = message_context;
    invalid_context.transport_id = "other-transport";

    if (!expect_status(
            context->v2->publish_authenticated(
                context->host_context,
                &invalid_context,
                nullptr,
                0
            ),
            MCP_PLUGIN_PERMISSION_DENIED)) {
        return MCP_PLUGIN_ERROR;
    }

#if defined(TEST_EXPECT_UNAUTHORIZED)
    const mcp_plugin_status unauthorized_status =
        context->v2->publish_authenticated(
            context->host_context,
            &message_context,
            nullptr,
            0
        );

    if (unauthorized_status != MCP_PLUGIN_PERMISSION_DENIED) {
        return MCP_PLUGIN_ERROR;
    }

    std::memcpy(result_buffer, RESULT, sizeof(RESULT));
    *result_size = sizeof(RESULT);
    return MCP_PLUGIN_OK;
#else
    std::uint8_t payload[] = {
        'a', 'u', 't', 'h', 'e', 'n', 't', 'i',
        'c', 'a', 't', 'e', 'd', '-',
        'p', 'a', 'y', 'l', 'o', 'a', 'd',
        '-', 'o', 'r', 'i', 'g', 'i', 'n', 'a', 'l'
    };

    const mcp_plugin_status status =
        context->v2->publish_authenticated(
            context->host_context,
            &message_context,
            payload,
            sizeof(payload)
        );

    if (status != MCP_PLUGIN_OK) {
        return status;
    }

    /*
     * Core must already own a copy of the payload.
     * Mutating this buffer must not affect the queued message.
     */
    std::memset(payload, 'X', sizeof(payload));

    // A zero-length payload is valid, including a null data pointer.
    const mcp_plugin_status empty_status =
    context->v2->publish_authenticated(
        context->host_context,
        &message_context,
        nullptr,
        0
    );

    if (empty_status != MCP_PLUGIN_OK) {
        return empty_status;
    }

    std::memcpy(result_buffer, RESULT, sizeof(RESULT));
    *result_size = sizeof(RESULT);

    return MCP_PLUGIN_OK;
#endif
}

const mcp_plugin_descriptor DESCRIPTOR = {
    MCP_PLUGIN_ABI_VERSION,
    sizeof(mcp_plugin_descriptor),
    TEST_PLUGIN_ID,
    "0.1.0",
    "MCP Core authenticated async v2 test plugin",
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
