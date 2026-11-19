#ifndef MCP_PLUGIN_MANAGER_HPP
#define MCP_PLUGIN_MANAGER_HPP

#include "mcp/plugin_abi.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mcp {

struct RegisteredTool {
    std::string plugin_id;
    std::string name;
    std::string description;
    std::string input_schema_json;
    std::vector<std::string> required_capabilities;
};

class PluginManager {
public:
    using PluginMessageHandler = std::function<void(
        const std::string& plugin_id,
        const std::vector<std::uint8_t>& message
    )>;

    PluginManager();
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    bool load_directory(
        const std::string& directory,
        const std::string& configuration_json = "{}"
    );

    bool execute_tool(
        const std::string& tool_name,
        const std::string& arguments_json,
        std::string& result,
        std::string& error
    );

    const std::vector<RegisteredTool>& tools() const;

    std::size_t plugin_count() const;
    std::size_t tool_count() const;

    /*
     * Registers or replaces the asynchronous Core message handler.
     * Must not be called from inside the message handler itself.
     */
    void set_message_handler(PluginMessageHandler handler);

    /*
     * Removes the Core message handler and waits until all
     * callbacks already in progress have completed.
     * Must not be called from inside the message handler itself.
     */
    void clear_message_handler();

    /*
     * Queues a message for delivery to a plugin's registered
     * asynchronous receiver. The input is copied before return.
     */
    bool send_plugin_message(
        const std::string& plugin_id,
        const std::uint8_t* data,
        std::size_t size,
        std::string& error
    );

    void shutdown();

private:
    struct PluginRecord;
    struct AsyncState;

    static void host_log(
        void* host_context,
        mcp_log_level level,
        const char* plugin_id,
        const char* message
    );

    static mcp_plugin_status host_get_extension(
        void* host_context,
        uint64_t extension_id,
        uint32_t minimum_version,
        const void** extension
    );

    static mcp_plugin_status host_register_receiver(
        void* host_context,
        void (*callback)(
            void* user_data,
            const uint8_t* data,
            size_t data_size
        ),
        void* user_data,
        void** subscription
    );

    static mcp_plugin_status host_unregister_receiver(
        void* host_context,
        void* subscription
    );

    static mcp_plugin_status host_publish(
        void* host_context,
        const uint8_t* data,
        size_t data_size
    );

    static void log_message(
        mcp_log_level level,
        const std::string& plugin_id,
        const std::string& message
    );

    bool load_plugin(
        const std::string& path,
        const std::string& configuration_json
    );

    std::unique_ptr<AsyncState> async_state_;

    std::vector<std::unique_ptr<PluginRecord>> plugins_;
    std::vector<RegisteredTool> tools_;

    std::size_t rejected_plugins_ = 0;
};

} // namespace mcp

#endif
