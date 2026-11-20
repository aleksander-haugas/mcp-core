#include "mcp/plugin_abi.h"

#include <stddef.h>
#include <stdint.h>

_Static_assert(
    MCP_PLUGIN_ABI_VERSION == MCP_PLUGIN_ABI_VERSION_1_0,
    "Global ABI version changed"
);

_Static_assert(
    MCP_PLUGIN_ASYNC_MESSAGES_VERSION ==
    MCP_PLUGIN_ASYNC_MESSAGES_VERSION_1,
    "Legacy async version alias changed"
);

_Static_assert(
    offsetof(mcp_async_messages_v2, publish_authenticated) >
    offsetof(mcp_async_messages_v2, publish),
    "Authenticated callback must extend the v1 prefix"
);

int main(void)
{
    mcp_async_messages_v1 legacy = {0};
    mcp_async_messages_v2 extended = {0};

    legacy.version = MCP_PLUGIN_ASYNC_MESSAGES_VERSION_1;
    extended.version = MCP_PLUGIN_ASYNC_MESSAGES_VERSION_2;

    return (
        legacy.version == 1u &&
        extended.version == 2u
    ) ? 0 : 1;
}
