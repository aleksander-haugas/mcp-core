#include "mcp/plugin_abi.h"

#include <cstddef>
#include <type_traits>

/*
 * The v2 extension must preserve the complete v1 binary prefix.
 */

static_assert(
    offsetof(mcp_async_messages_v1, struct_size) ==
    offsetof(mcp_async_messages_v2, struct_size),
    "struct_size offset changed"
);

static_assert(
    offsetof(mcp_async_messages_v1, version) ==
    offsetof(mcp_async_messages_v2, version),
    "version offset changed"
);

static_assert(
    offsetof(mcp_async_messages_v1, register_receiver) ==
    offsetof(mcp_async_messages_v2, register_receiver),
    "register_receiver offset changed"
);

static_assert(
    offsetof(mcp_async_messages_v1, unregister_receiver) ==
    offsetof(mcp_async_messages_v2, unregister_receiver),
    "unregister_receiver offset changed"
);

static_assert(
    offsetof(mcp_async_messages_v1, publish) ==
    offsetof(mcp_async_messages_v2, publish),
    "publish offset changed"
);

static_assert(
    offsetof(mcp_async_messages_v2, publish_authenticated) ==
    sizeof(mcp_async_messages_v1),
    "v2 authenticated callback must follow the complete v1 prefix"
);

static_assert(
    sizeof(mcp_async_messages_v2) >
    sizeof(mcp_async_messages_v1),
    "v2 must extend v1"
);

static_assert(
    std::is_standard_layout<mcp_async_messages_v1>::value,
    "v1 must remain standard-layout"
);

static_assert(
    std::is_standard_layout<mcp_async_messages_v2>::value,
    "v2 must remain standard-layout"
);

static_assert(
    std::is_same<
        decltype(mcp_async_messages_v1::register_receiver),
        decltype(mcp_async_messages_v2::register_receiver)
    >::value,
    "register_receiver signature changed"
);

static_assert(
    std::is_same<
        decltype(mcp_async_messages_v1::unregister_receiver),
        decltype(mcp_async_messages_v2::unregister_receiver)
    >::value,
    "unregister_receiver signature changed"
);

static_assert(
    std::is_same<
        decltype(mcp_async_messages_v1::publish),
        decltype(mcp_async_messages_v2::publish)
    >::value,
    "publish signature changed"
);

static_assert(
    MCP_PLUGIN_ABI_VERSION == MCP_PLUGIN_ABI_VERSION_1_0,
    "Global ABI must remain at version 1.0"
);

static_assert(
    MCP_PLUGIN_ASYNC_MESSAGES_VERSION ==
    MCP_PLUGIN_ASYNC_MESSAGES_VERSION_1,
    "Legacy async version alias must remain v1"
);

int main()
{
    return 0;
}
