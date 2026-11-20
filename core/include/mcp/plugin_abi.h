#ifndef MCP_PLUGIN_ABI_H
#define MCP_PLUGIN_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MCP Core Plugin ABI
 *
 * Base ABI: 1.0
 * Optional host extensions: versioned independently.
 * ABI 1.0 plugins remain supported.
 * Language interface: C
 *
 * Plugins must not depend on C++ object layouts or exceptions
 * crossing the ABI boundary.
 */

#define MCP_PLUGIN_ABI_VERSION 0x00010000u

#if defined(__GNUC__) || defined(__clang__)
#define MCP_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define MCP_PLUGIN_EXPORT
#endif

#define MCP_PLUGIN_ENTRY_SYMBOL "mcp_plugin_query"

/* ---------------------------------------------------------
 * Logging
 * --------------------------------------------------------- */

typedef enum mcp_log_level {
    MCP_LOG_DEBUG = 0,
    MCP_LOG_INFO = 1,
    MCP_LOG_WARNING = 2,
    MCP_LOG_ERROR = 3,
    MCP_LOG_CRITICAL = 4
} mcp_log_level;

/* ---------------------------------------------------------
 * Status codes
 * --------------------------------------------------------- */

typedef enum mcp_plugin_status {
    MCP_PLUGIN_OK = 0,
    MCP_PLUGIN_ERROR = 1,
    MCP_PLUGIN_BUFFER_TOO_SMALL = 2,
    MCP_PLUGIN_INVALID_ARGUMENT = 3,
    MCP_PLUGIN_NOT_FOUND = 4,
    MCP_PLUGIN_PERMISSION_DENIED = 5,
    MCP_PLUGIN_NOT_SUPPORTED = 6
} mcp_plugin_status;

typedef mcp_plugin_status (*mcp_plugin_get_extension_fn)(
    void *host_context,
    uint64_t extension_id,
    uint32_t minimum_version,
    const void **extension
);

/* ---------------------------------------------------------
 * Host API
 * --------------------------------------------------------- */

typedef struct mcp_host_api {
    uint32_t abi_version;
    uint32_t struct_size;

    void *host_context;

    void (*log)(
        void *host_context,
        mcp_log_level level,
        const char *plugin_id,
        const char *message
    );

    mcp_plugin_get_extension_fn get_extension;

} mcp_host_api;


/* ---------------------------------------------------------
 * Optional host extensions
 * --------------------------------------------------------- */

#define MCP_PLUGIN_ABI_VERSION_1_0 0x00010000u
#define MCP_PLUGIN_ABI_VERSION_1_1 0x00010001u

#define MCP_PLUGIN_EXT_ASYNC_MESSAGES 0x4D435001ULL
#define MCP_PLUGIN_ASYNC_MESSAGES_VERSION_1 1u
#define MCP_PLUGIN_ASYNC_MESSAGES_VERSION_2 2u
#define MCP_PLUGIN_AUTHENTICATED_MESSAGE_CONTEXT_VERSION_1 1u
#define MCP_PLUGIN_ASYNC_MESSAGES_VERSION \
    MCP_PLUGIN_ASYNC_MESSAGES_VERSION_1

typedef struct mcp_async_messages_v1 {
    uint32_t struct_size;
    uint32_t version;

    /*
     * Plugin registers a callback for messages sent by Core.
     * The callback buffer is valid only during the callback.
     */
    mcp_plugin_status (*register_receiver)(
        void *host_context,
        void (*callback)(
            void *user_data,
            const uint8_t *data,
            size_t data_size
        ),
        void *user_data,
        void **subscription
    );

    /*
     * Prevents new deliveries and waits for active callbacks.
     * Must not be called from the registered callback itself.
     */
    mcp_plugin_status (*unregister_receiver)(
        void *host_context,
        void *subscription
    );

    /*
     * Publishes an inbound message from the plugin to Core.
     * Core copies the supplied bytes before returning.
     */
    mcp_plugin_status (*publish)(
        void *host_context,
        const uint8_t *data,
        size_t data_size
    );

} mcp_async_messages_v1;

/*
 * Authenticated message metadata.
 *
 * Strings are borrowed and valid only during publish_authenticated().
 * The host must copy them before returning.
 */
typedef struct mcp_authenticated_message_context {
    uint32_t struct_size;
    uint32_t version;
    const char *transport_id;
    const char *authenticated_principal;
} mcp_authenticated_message_context;

typedef struct mcp_async_messages_v2 {
    uint32_t struct_size;
    uint32_t version;

    /* v1-compatible function prefix. */
    mcp_plugin_status (*register_receiver)(
        void *host_context,
        void (*callback)(
            void *user_data,
            const uint8_t *data,
            size_t data_size
        ),
        void *user_data,
        void **subscription
    );

    mcp_plugin_status (*unregister_receiver)(
        void *host_context,
        void *subscription
    );

    mcp_plugin_status (*publish)(
        void *host_context,
        const uint8_t *data,
        size_t data_size
    );

    mcp_plugin_status (*publish_authenticated)(
        void *host_context,
        const mcp_authenticated_message_context *context,
        const uint8_t *data,
        size_t data_size
    );

} mcp_async_messages_v2;


/*
 * Optional extension lookup.
 *
 * The host returns a pointer to an immutable extension table.
 * The table remains valid until the plugin is shut down.
 */


/* ---------------------------------------------------------
 * Tool definition
 * --------------------------------------------------------- */

typedef struct mcp_tool_definition {
    const char *name;
    const char *description;

    /*
     * JSON Schema describing the tool arguments.
     * Must remain valid for the plugin lifetime.
     */
    const char *input_schema_json;

    /*
     * Capabilities required by this tool.
     */
    const char *const *required_capabilities;
    size_t required_capabilities_count;

} mcp_tool_definition;

/* ---------------------------------------------------------
 * Plugin lifecycle callbacks
 * --------------------------------------------------------- */

typedef mcp_plugin_status (*mcp_plugin_initialize_fn)(
    const mcp_host_api *host,
    const char *configuration_json,
    void **plugin_context
);

typedef void (*mcp_plugin_shutdown_fn)(
    void *plugin_context
);

/* ---------------------------------------------------------
 * Tool registry callbacks
 * --------------------------------------------------------- */

typedef mcp_plugin_status (*mcp_plugin_list_tools_fn)(
    void *plugin_context,
    const mcp_tool_definition **tools,
    size_t *tool_count
);

/* ---------------------------------------------------------
 * Tool execution callback
 * --------------------------------------------------------- */

/*
 * arguments_json:
 *     Valid JSON object containing tool arguments.
 *
 * result_buffer:
 *     Caller-owned output buffer.
 *
 * result_size:
 *     On input: buffer capacity in bytes.
 *     On output: required size in bytes, including '\0'.
 *
 * Two-pass operation:
 *     1. Call with result_buffer = NULL.
 *     2. Allocate the required buffer.
 *     3. Call again with the allocated buffer.
 *
 * The returned result must be valid JSON.
 */

typedef mcp_plugin_status (*mcp_plugin_execute_tool_fn)(
    void *plugin_context,
    const char *tool_name,
    const char *arguments_json,
    char *result_buffer,
    size_t *result_size
);

/* ---------------------------------------------------------
 * Plugin descriptor
 * --------------------------------------------------------- */

typedef struct mcp_plugin_descriptor {
    uint32_t abi_version;
    uint32_t struct_size;

    const char *plugin_id;
    const char *plugin_version;
    const char *description;

    mcp_plugin_initialize_fn initialize;
    mcp_plugin_shutdown_fn shutdown;

    mcp_plugin_list_tools_fn list_tools;
    mcp_plugin_execute_tool_fn execute_tool;

} mcp_plugin_descriptor;

/* ---------------------------------------------------------
 * Mandatory plugin entry point
 * --------------------------------------------------------- */

/*
 * Every plugin must export:
 *
 *     mcp_plugin_query
 *
 * The returned descriptor must remain valid for the
 * entire lifetime of the loaded shared library.
 */

typedef const mcp_plugin_descriptor *(*mcp_plugin_query_fn)(
    void
);

MCP_PLUGIN_EXPORT
const mcp_plugin_descriptor *mcp_plugin_query(void);

#ifdef __cplusplus
}
#endif

#endif
