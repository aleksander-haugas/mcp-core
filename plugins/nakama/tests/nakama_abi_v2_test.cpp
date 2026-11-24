#include "mcp/plugin_abi.h"

#include <dlfcn.h>
#include <json-c/json.h>

#include <cstdio>
#include <cstring>

namespace {

int checks = 0;
int failures = 0;

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

struct HostState {
    void (*receiver)(
        void*,
        const uint8_t*,
        size_t
    ) = nullptr;

    void* receiver_user_data = nullptr;
    int registration_calls = 0;
    int unregistration_calls = 0;
    bool accepting = true;
    int subscription_marker = 0;
};

void host_log(
    void*,
    mcp_log_level,
    const char*,
    const char*
)
{
}

mcp_plugin_status register_receiver(
    void* host_context,
    void (*callback)(
        void*,
        const uint8_t*,
        size_t
    ),
    void* user_data,
    void** subscription
)
{
    auto* host = static_cast<HostState*>(host_context);

    if (host == nullptr ||
        callback == nullptr ||
        subscription == nullptr ||
        !host->accepting ||
        host->receiver != nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    host->receiver = callback;
    host->receiver_user_data = user_data;
    ++host->registration_calls;

    *subscription = &host->subscription_marker;
    return MCP_PLUGIN_OK;
}

mcp_plugin_status unregister_receiver(
    void* host_context,
    void* subscription
)
{
    auto* host = static_cast<HostState*>(host_context);

    if (host == nullptr ||
        subscription != &host->subscription_marker) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    host->accepting = false;
    host->receiver = nullptr;
    host->receiver_user_data = nullptr;
    ++host->unregistration_calls;

    return MCP_PLUGIN_OK;
}

mcp_plugin_status publish(
    void*,
    const uint8_t* data,
    size_t data_size
)
{
    if (data == nullptr || data_size == 0) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    return MCP_PLUGIN_OK;
}

mcp_plugin_status publish_authenticated(
    void*,
    const mcp_authenticated_message_context* context,
    const uint8_t* data,
    size_t data_size
)
{
    if (context == nullptr ||
        data == nullptr ||
        data_size == 0) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    return MCP_PLUGIN_OK;
}

const mcp_async_messages_v2 ASYNC_MESSAGES_V2 = {
    sizeof(mcp_async_messages_v2),
    MCP_PLUGIN_ASYNC_MESSAGES_VERSION_2,
    register_receiver,
    unregister_receiver,
    publish,
    publish_authenticated
};

mcp_plugin_status get_extension(
    void* host_context,
    uint64_t extension_id,
    uint32_t minimum_version,
    const void** extension
)
{
    if (extension == nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    *extension = nullptr;

    if (extension_id != MCP_PLUGIN_EXT_ASYNC_MESSAGES ||
        minimum_version > MCP_PLUGIN_ASYNC_MESSAGES_VERSION_2) {
        return MCP_PLUGIN_NOT_SUPPORTED;
    }

    if (host_context == nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    *extension = &ASYNC_MESSAGES_V2;
    return MCP_PLUGIN_OK;
}

bool status_has_error(
    const mcp_plugin_descriptor* descriptor,
    void* plugin_context,
    const char* expected_error
)
{
    char result[8192] = {};
    size_t result_size = sizeof(result);

    const mcp_plugin_status status = descriptor->execute_tool(
        plugin_context,
        "nakama_status",
        "{}",
        result,
        &result_size
    );

    if (status != MCP_PLUGIN_OK) {
        return false;
    }

    json_object* root = json_tokener_parse(result);

    if (root == nullptr) {
        return false;
    }

    json_object* error = nullptr;

    const bool matches =
        json_object_object_get_ex(root, "last_error", &error) &&
        error != nullptr &&
        json_object_get_type(error) == json_type_string &&
        std::strcmp(
            json_object_get_string(error),
            expected_error
        ) == 0;

    json_object_put(root);
    return matches;
}

bool run_v2_scenario(const mcp_plugin_descriptor* descriptor)
{
    HostState host_state{};

    mcp_host_api host{};
    host.abi_version = MCP_PLUGIN_ABI_VERSION;
    host.struct_size = sizeof(host);
    host.host_context = &host_state;
    host.log = host_log;
    host.get_extension = get_extension;

    void* plugin_context = nullptr;

    const mcp_plugin_status init_status =
        descriptor->initialize(&host, "{}", &plugin_context);

    check(
        init_status == MCP_PLUGIN_OK,
        "ABI v2 plugin initialization succeeds"
    );

    check(
        plugin_context != nullptr,
        "ABI v2 plugin context is created"
    );

    if (init_status != MCP_PLUGIN_OK || plugin_context == nullptr) {
        return false;
    }

    check(
        host_state.registration_calls == 1,
        "Exactly one Core-to-Nakama receiver is registered"
    );

    check(
        host_state.receiver != nullptr &&
        host_state.receiver_user_data != nullptr,
        "Host captures receiver callback and user data"
    );

    if (host_state.receiver != nullptr) {
        static const uint8_t message[] = {'{', '}'};

        host_state.receiver(
            host_state.receiver_user_data,
            message,
            sizeof(message)
        );

        check(
            status_has_error(
                descriptor,
                plugin_context,
                "Core message could not be delivered: Nakama is disconnected"
            ),
            "Disconnected delivery is reported through nakama_status"
        );
    } else {
        check(false, "Disconnected delivery callback is available");
    }

    // Simulate PluginManager deactivation:
    // prevent new callbacks and drain active callbacks before shutdown.
    host_state.accepting = false;

    descriptor->shutdown(plugin_context);

    check(
        host_state.unregistration_calls == 0,
        "Plugin shutdown does not unregister an already-drained receiver"
    );

    return true;
}

bool run_fallback_scenario(
    const mcp_plugin_descriptor* descriptor
)
{
    HostState host_state{};

    mcp_host_api host{};
    host.abi_version = MCP_PLUGIN_ABI_VERSION;
    host.struct_size = sizeof(host);
    host.host_context = &host_state;
    host.log = host_log;

    host.get_extension = [](
        void*,
        uint64_t,
        uint32_t,
        const void** extension
    ) -> mcp_plugin_status {
        if (extension != nullptr) {
            *extension = nullptr;
        }
        return MCP_PLUGIN_NOT_SUPPORTED;
    };

    void* plugin_context = nullptr;

    const mcp_plugin_status init_status =
        descriptor->initialize(&host, "{}", &plugin_context);

    check(
        init_status == MCP_PLUGIN_OK,
        "Plugin initializes when async ABI v2 is unsupported"
    );

    check(
        plugin_context != nullptr,
        "Fallback initialization creates plugin context"
    );

    check(
        host_state.registration_calls == 0,
        "Fallback does not register an async receiver"
    );

    if (plugin_context != nullptr) {
        descriptor->shutdown(plugin_context);
    }

    return init_status == MCP_PLUGIN_OK &&
           plugin_context != nullptr;
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

    check(library != nullptr, "Nakama shared library loaded");

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
        "Plugin query symbol resolved"
    );

    if (query == nullptr || symbol_error != nullptr) {
        dlclose(library);
        return 1;
    }

    const mcp_plugin_descriptor* descriptor = query();

    check(
        descriptor != nullptr,
        "Plugin descriptor returned"
    );

    if (descriptor == nullptr ||
        descriptor->initialize == nullptr ||
        descriptor->shutdown == nullptr ||
        descriptor->execute_tool == nullptr) {
        dlclose(library);
        return 1;
    }

    run_v2_scenario(descriptor);
    run_fallback_scenario(descriptor);

    check(
        dlclose(library) == 0,
        "Nakama shared library unloaded"
    );

    std::printf(
        "\nABI v2 checks: %d, failures: %d\n",
        checks,
        failures
    );

    return failures == 0 ? 0 : 1;
}
