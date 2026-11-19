#include "mcp/plugin_manager.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <unordered_map>

namespace mcp {

namespace {

constexpr std::size_t MAX_PLUGIN_TOOLS = 1024;
constexpr std::size_t MAX_PLUGIN_CAPABILITIES = 64;
constexpr std::size_t MAX_RESULT_SIZE = 16 * 1024 * 1024;

const char* status_name(mcp_plugin_status status)
{
    switch (status) {
        case MCP_PLUGIN_OK:
            return "OK";
        case MCP_PLUGIN_ERROR:
            return "ERROR";
        case MCP_PLUGIN_BUFFER_TOO_SMALL:
            return "BUFFER_TOO_SMALL";
        case MCP_PLUGIN_INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case MCP_PLUGIN_NOT_FOUND:
            return "NOT_FOUND";
        case MCP_PLUGIN_PERMISSION_DENIED:
            return "PERMISSION_DENIED";
        case MCP_PLUGIN_NOT_SUPPORTED:
            return "NOT_SUPPORTED";
        default:
            return "UNKNOWN";
    }
}

bool valid_string(const char* value)
{
    return value != nullptr && value[0] != '\0';
}

} // namespace

struct PluginManager::AsyncState {
    static constexpr std::size_t MAX_QUEUE_SIZE = 1024;
    static constexpr std::size_t MAX_MESSAGE_SIZE = 1024 * 1024;

    struct HostContext;

    struct Subscription {
        std::string plugin_id;
        HostContext* owner = nullptr;

        void (*callback)(
            void* user_data,
            const uint8_t* data,
            size_t data_size
        ) = nullptr;

        void* user_data = nullptr;

        bool active = true;
        std::size_t active_callbacks = 0;

        std::condition_variable drained;
    };

    struct HostContext {
        AsyncState* state = nullptr;
        std::string plugin_id;
        bool accepting = true;
        bool ready = false;
    };

    struct QueueItem {
        enum class Direction {
            TO_PLUGIN,
            TO_CORE
        };

        Direction direction = Direction::TO_CORE;
        std::string plugin_id;
        std::vector<std::uint8_t> data;
        std::shared_ptr<Subscription> subscription;
    };

    std::mutex mutex;
    std::condition_variable available;

    std::deque<QueueItem> to_plugin_queue;
    std::deque<QueueItem> to_core_queue;

    std::unordered_map<
        void*,
        std::shared_ptr<Subscription>
    > subscriptions;

    PluginMessageHandler message_handler;
    std::size_t active_core_callbacks = 0;
    std::condition_variable core_callbacks_drained;
    bool core_handler_draining = false;

    bool stopping = false;
    bool prefer_core_queue = true;
    std::thread worker;

    inline static thread_local const Subscription*
        executing_subscription = nullptr;

    inline static thread_local const AsyncState*
        executing_core_handler = nullptr;

    AsyncState()
        : worker(&AsyncState::run, this)
    {
    }

    ~AsyncState()
    {
        stop_worker();
    }

    mcp_plugin_status register_receiver(
        HostContext& host,
        void (*callback)(
            void* user_data,
            const uint8_t* data,
            size_t data_size
        ),
        void* user_data,
        void** handle
    )
    {
        if (handle == nullptr) {
            return MCP_PLUGIN_INVALID_ARGUMENT;
        }

        *handle = nullptr;

        if (callback == nullptr) {
            return MCP_PLUGIN_INVALID_ARGUMENT;
        }

        std::lock_guard<std::mutex> lock(mutex);

        if (stopping || !host.accepting) {
            return MCP_PLUGIN_ERROR;
        }

        if (subscriptions.size() >= MAX_QUEUE_SIZE) {
            return MCP_PLUGIN_NOT_SUPPORTED;
        }

        try {
            auto subscription = std::make_shared<Subscription>();

            subscription->plugin_id = host.plugin_id;
            subscription->owner = &host;
            subscription->callback = callback;
            subscription->user_data = user_data;

            void* token = subscription.get();

            subscriptions.emplace(token, subscription);
            *handle = token;

            return MCP_PLUGIN_OK;
        } catch (...) {
            return MCP_PLUGIN_ERROR;
        }
    }

    bool activate_plugin(HostContext& host)
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (stopping || !host.accepting) {
            return false;
        }

        host.ready = true;
        available.notify_all();
        return true;
    }

    void deactivate_plugin(HostContext& host)
    {
        std::unique_lock<std::mutex> lock(mutex);

        host.accepting = false;
        host.ready = false;

        // Remove messages that have not yet been delivered to this plugin.
        to_plugin_queue.erase(
            std::remove_if(
                to_plugin_queue.begin(),
                to_plugin_queue.end(),
                [&host](const QueueItem& item) {
                    return item.subscription &&
                        item.subscription->owner == &host;
                }
            ),
            to_plugin_queue.end()
        );

        // Do not leave queued Core messages attributed to a plugin
        // that is being unloaded.
        to_core_queue.erase(
            std::remove_if(
                to_core_queue.begin(),
                to_core_queue.end(),
                [&host](const QueueItem& item) {
                    return item.plugin_id == host.plugin_id;
                }
            ),
            to_core_queue.end()
        );

        // Erase each handle before waiting. The local shared_ptr keeps
        // its subscription alive until all active callbacks have drained.
        for (;;) {
            auto found = std::find_if(
                subscriptions.begin(),
                subscriptions.end(),
                [&host](const auto& entry) {
                    return entry.second->owner == &host;
                }
            );

            if (found == subscriptions.end()) {
                break;
            }

            auto subscription = found->second;
            subscription->active = false;
            subscriptions.erase(found);

            subscription->drained.wait(
                lock,
                [&subscription] {
                    return subscription->active_callbacks == 0;
                }
            );
        }
    }

    mcp_plugin_status unregister_receiver(
        HostContext& host,
        void* handle
    )
    {
        if (handle == nullptr) {
            return MCP_PLUGIN_INVALID_ARGUMENT;
        }

        if (executing_subscription == handle) {
            return MCP_PLUGIN_INVALID_ARGUMENT;
        }

        std::unique_lock<std::mutex> lock(mutex);

        auto found = subscriptions.find(handle);

        if (found == subscriptions.end() ||
            found->second->plugin_id != host.plugin_id) {
            return MCP_PLUGIN_NOT_FOUND;
        }

        auto subscription = found->second;

        subscription->active = false;

        to_plugin_queue.erase(
            std::remove_if(
                to_plugin_queue.begin(),
                to_plugin_queue.end(),
                [&subscription](const QueueItem& item) {
                    return item.subscription == subscription;
                }
            ),
            to_plugin_queue.end()
        );

        // Remove the handle before waiting. The local shared_ptr keeps
        // the subscription alive while active callbacks drain.
        subscriptions.erase(found);

        subscription->drained.wait(lock, [&subscription] {
            return subscription->active_callbacks == 0;
        });

        return MCP_PLUGIN_OK;
    }

    mcp_plugin_status publish(
        HostContext& host,
        const uint8_t* data,
        size_t size
    )
    {
        if ((data == nullptr && size != 0) ||
            size > MAX_MESSAGE_SIZE) {
            return MCP_PLUGIN_INVALID_ARGUMENT;
        }

        std::lock_guard<std::mutex> lock(mutex);

        if (stopping || !host.accepting) {
            return MCP_PLUGIN_ERROR;
        }

        if (to_plugin_queue.size() + to_core_queue.size() >=
            MAX_QUEUE_SIZE) {
            return MCP_PLUGIN_NOT_SUPPORTED;
        }

        try {
            QueueItem item;
            item.direction = QueueItem::Direction::TO_CORE;
            item.plugin_id = host.plugin_id;

            if (size != 0) {
                item.data.assign(data, data + size);
            }

            to_core_queue.push_back(std::move(item));
        } catch (...) {
            return MCP_PLUGIN_ERROR;
        }

        available.notify_one();
        return MCP_PLUGIN_OK;
    }

    mcp_plugin_status enqueue_for_plugin(
        const std::string& plugin_id,
        const uint8_t* data,
        size_t size
    )
    {
        if (plugin_id.empty() ||
            (data == nullptr && size != 0) ||
            size > MAX_MESSAGE_SIZE) {
            return MCP_PLUGIN_INVALID_ARGUMENT;
        }

        std::lock_guard<std::mutex> lock(mutex);

        if (stopping) {
            return MCP_PLUGIN_ERROR;
        }

        try {
            std::vector<std::shared_ptr<Subscription>> targets;

            for (const auto& entry : subscriptions) {
                const auto& subscription = entry.second;

                if (subscription->active &&
                    subscription->owner != nullptr &&
                    subscription->owner->ready &&
                    subscription->plugin_id == plugin_id) {
                    targets.push_back(subscription);
                }
            }

            if (targets.empty()) {
                return MCP_PLUGIN_NOT_FOUND;
            }

            const std::size_t queued =
                to_plugin_queue.size() + to_core_queue.size();

            if (queued >= MAX_QUEUE_SIZE ||
                targets.size() > MAX_QUEUE_SIZE - queued) {
                return MCP_PLUGIN_NOT_SUPPORTED;
            }

            std::vector<QueueItem> pending;
            pending.reserve(targets.size());

            for (const auto& subscription : targets) {
                QueueItem item;
                item.direction = QueueItem::Direction::TO_PLUGIN;
                item.plugin_id = plugin_id;
                item.subscription = subscription;

                if (size != 0) {
                    item.data.assign(data, data + size);
                }

                pending.push_back(std::move(item));
            }

            // The worker cannot consume while mutex is held. If a deque
            // allocation fails, remove every item appended by this call.
            const std::size_t original_size = to_plugin_queue.size();

            try {
                for (auto& item : pending) {
                    to_plugin_queue.push_back(std::move(item));
                }
            } catch (...) {
                while (to_plugin_queue.size() > original_size) {
                    to_plugin_queue.pop_back();
                }
                return MCP_PLUGIN_ERROR;
            }
        } catch (...) {
            return MCP_PLUGIN_ERROR;
        }

        available.notify_one();
        return MCP_PLUGIN_OK;
    }

    void stop_worker() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex);

            stopping = true;
            to_plugin_queue.clear();
            to_core_queue.clear();

            for (auto& entry : subscriptions) {
                entry.second->active = false;
            }
        }

        available.notify_all();

        if (worker.joinable() &&
            worker.get_id() != std::this_thread::get_id()) {
            worker.join();
        }
    }

    void run() noexcept
    {
        for (;;) {
            QueueItem item;
            PluginMessageHandler handler;
            bool dispatch_to_plugin = false;

            {
                std::unique_lock<std::mutex> lock(mutex);

                available.wait(lock, [this] {
                    return stopping ||
                        !to_plugin_queue.empty() ||
                        (!to_core_queue.empty() &&
                         static_cast<bool>(message_handler));
                });

                if (stopping) {
                    return;
                }

                const bool can_dispatch_plugin =
                    !to_plugin_queue.empty();

                const bool can_dispatch_core =
                    !to_core_queue.empty() &&
                    static_cast<bool>(message_handler);

                if (can_dispatch_core &&
                    (!can_dispatch_plugin || prefer_core_queue)) {

                    item = std::move(to_core_queue.front());
                    to_core_queue.pop_front();
                    item.direction = QueueItem::Direction::TO_CORE;
                    prefer_core_queue = false;

                } else if (can_dispatch_plugin) {

                    item = std::move(to_plugin_queue.front());
                    to_plugin_queue.pop_front();
                    item.direction = QueueItem::Direction::TO_PLUGIN;
                    prefer_core_queue = true;

                } else {
                    continue;
                }

                if (item.direction ==
                    QueueItem::Direction::TO_PLUGIN) {

                    if (!item.subscription ||
                        !item.subscription->active ||
                        item.subscription->owner == nullptr ||
                        !item.subscription->owner->ready) {
                        continue;
                    }

                    ++item.subscription->active_callbacks;
                    dispatch_to_plugin = true;
                } else {
                    handler = message_handler;
                    if (handler) {
                        ++active_core_callbacks;
                    }
                }
            }

            if (dispatch_to_plugin) {
                const Subscription* previous_subscription =
                    executing_subscription;

                executing_subscription = item.subscription.get();

                try {
                    item.subscription->callback(
                        item.subscription->user_data,
                        item.data.data(),
                        item.data.size()
                    );
                } catch (...) {
                    PluginManager::log_message(
                        MCP_LOG_ERROR,
                        item.plugin_id,
                        "Exception in asynchronous plugin receiver"
                    );
                }

                executing_subscription = previous_subscription;

                {
                    std::lock_guard<std::mutex> lock(mutex);

                    if (item.subscription->active_callbacks > 0) {
                        --item.subscription->active_callbacks;
                    }

                    if (item.subscription->active_callbacks == 0) {
                        item.subscription->drained.notify_all();
                    }
                }
            } else if (handler) {
                const AsyncState* previous_handler =
                    executing_core_handler;
                executing_core_handler = this;

                try {
                    handler(item.plugin_id, item.data);
                } catch (...) {
                    PluginManager::log_message(
                        MCP_LOG_ERROR,
                        item.plugin_id,
                        "Exception in Core plugin message handler"
                    );
                }

                executing_core_handler = previous_handler;

                {
                    std::lock_guard<std::mutex> lock(mutex);

                    if (active_core_callbacks > 0) {
                        --active_core_callbacks;
                    }

                    if (active_core_callbacks == 0) {
                        core_callbacks_drained.notify_all();
                    }
                }
            }
        }
    }
};

struct PluginManager::PluginRecord {
    void* library_handle = nullptr;

    const mcp_plugin_descriptor* descriptor = nullptr;

    void* plugin_context = nullptr;

    std::shared_ptr<AsyncState::HostContext> host_context;

    mcp_host_api host_api{};

    std::string id;
    std::string path;

    bool initialized = false;
};

PluginManager::PluginManager()
    : async_state_(std::make_unique<AsyncState>())
{
}

PluginManager::~PluginManager()
{
    shutdown();
}

void PluginManager::log_message(
    mcp_log_level level,
    const std::string& plugin_id,
    const std::string& message
)
{
    const char* level_name = "UNKNOWN";

    switch (level) {
        case MCP_LOG_DEBUG:
            level_name = "DEBUG";
            break;
        case MCP_LOG_INFO:
            level_name = "INFO";
            break;
        case MCP_LOG_WARNING:
            level_name = "WARNING";
            break;
        case MCP_LOG_ERROR:
            level_name = "ERROR";
            break;
        case MCP_LOG_CRITICAL:
            level_name = "CRITICAL";
            break;
    }

    std::cerr
        << "[MCP][" << level_name << "]";

    if (!plugin_id.empty()) {
        std::cerr << "[" << plugin_id << "]";
    }

    std::cerr << " " << message << '\n';
}

void PluginManager::host_log(
    void* host_context,
    mcp_log_level level,
    const char* plugin_id,
    const char* message
)
{
    (void)plugin_id;

    if (host_context == nullptr || message == nullptr) {
        return;
    }

    auto* host =
        static_cast<AsyncState::HostContext*>(host_context);

    if (host->state == nullptr) {
        return;
    }

    log_message(level, host->plugin_id, message);
}

mcp_plugin_status PluginManager::host_get_extension(
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

    if (host_context == nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    auto* host =
        static_cast<AsyncState::HostContext*>(host_context);

    if (host->state == nullptr) {
        return MCP_PLUGIN_ERROR;
    }

    if (extension_id != MCP_PLUGIN_EXT_ASYNC_MESSAGES) {
        return MCP_PLUGIN_NOT_SUPPORTED;
    }

    if (minimum_version > MCP_PLUGIN_ASYNC_MESSAGES_VERSION) {
        return MCP_PLUGIN_NOT_SUPPORTED;
    }

    static const mcp_async_messages_v1 async_messages = {
        sizeof(mcp_async_messages_v1),
        MCP_PLUGIN_ASYNC_MESSAGES_VERSION,
        &PluginManager::host_register_receiver,
        &PluginManager::host_unregister_receiver,
        &PluginManager::host_publish
    };

    *extension = &async_messages;
    return MCP_PLUGIN_OK;
}

mcp_plugin_status PluginManager::host_register_receiver(
    void* host_context,
    void (*callback)(
        void* user_data,
        const uint8_t* data,
        size_t data_size
    ),
    void* user_data,
    void** subscription
)
{
    if (host_context == nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    auto* host =
        static_cast<AsyncState::HostContext*>(host_context);

    if (host->state == nullptr) {
        return MCP_PLUGIN_ERROR;
    }

    return host->state->register_receiver(
        *host,
        callback,
        user_data,
        subscription
    );
}

mcp_plugin_status PluginManager::host_unregister_receiver(
    void* host_context,
    void* subscription
)
{
    if (host_context == nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    auto* host =
        static_cast<AsyncState::HostContext*>(host_context);

    if (host->state == nullptr) {
        return MCP_PLUGIN_ERROR;
    }

    return host->state->unregister_receiver(
        *host,
        subscription
    );
}

mcp_plugin_status PluginManager::host_publish(
    void* host_context,
    const uint8_t* data,
    size_t data_size
)
{
    if (host_context == nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    auto* host =
        static_cast<AsyncState::HostContext*>(host_context);

    if (host->state == nullptr) {
        return MCP_PLUGIN_ERROR;
    }

    return host->state->publish(
        *host,
        data,
        data_size
    );
}

bool PluginManager::load_directory(
    const std::string& directory,
    const std::string& configuration_json
)
{
    namespace fs = std::filesystem;

    std::error_code ec;

    if (!fs::exists(directory, ec) ||
        ec ||
        !fs::is_directory(directory, ec) ||
        ec) {

        log_message(
            MCP_LOG_ERROR,
            "",
            "Plugin directory does not exist or is inaccessible: " +
                directory
        );

        return false;
    }

    std::vector<fs::path> libraries;

    fs::directory_iterator iterator(
        directory,
        fs::directory_options::skip_permission_denied,
        ec
    );

    if (ec) {
        log_message(
            MCP_LOG_ERROR,
            "",
            "Cannot scan plugin directory: " + ec.message()
        );

        return false;
    }

    const fs::directory_iterator end;

    while (iterator != end) {
        const fs::directory_entry entry = *iterator;

        std::error_code entry_error;

        if (entry.is_regular_file(entry_error) &&
            !entry_error &&
            entry.path().extension() == ".so") {

            libraries.push_back(entry.path());
        }

        iterator.increment(ec);

        if (ec) {
            log_message(
                MCP_LOG_ERROR,
                "",
                "Error while scanning plugin directory: " +
                    ec.message()
            );

            return false;
        }
    }

    std::sort(
        libraries.begin(),
        libraries.end()
    );

    log_message(
        MCP_LOG_INFO,
        "",
        "Discovered " + std::to_string(libraries.size()) +
            " shared libraries"
    );

    for (const auto& library : libraries) {
        if (!load_plugin(
                library.string(),
                configuration_json
            )) {

            ++rejected_plugins_;
        }
    }

    log_message(
        MCP_LOG_INFO,
        "",
        "Plugin loading finished. Loaded: " +
            std::to_string(plugins_.size()) +
            ", rejected: " +
            std::to_string(rejected_plugins_)
    );

    return true;
}

bool PluginManager::load_plugin(
    const std::string& path,
    const std::string& configuration_json
)
{
    auto record = std::make_unique<PluginRecord>();

    record->path = path;

    record->library_handle = dlopen(
        path.c_str(),
        RTLD_NOW | RTLD_LOCAL
    );

    if (record->library_handle == nullptr) {
        const char* error = dlerror();

        log_message(
            MCP_LOG_ERROR,
            "",
            "dlopen failed for " + path + ": " +
                (error != nullptr ? error : "unknown error")
        );

        return false;
    }

    dlerror();

    void* symbol = dlsym(
        record->library_handle,
        MCP_PLUGIN_ENTRY_SYMBOL
    );

    const char* symbol_error = dlerror();

    if (symbol_error != nullptr || symbol == nullptr) {
        log_message(
            MCP_LOG_ERROR,
            "",
            "Missing plugin entry point in " + path
        );

        dlclose(record->library_handle);
        record->library_handle = nullptr;

        return false;
    }

    auto query = reinterpret_cast<mcp_plugin_query_fn>(symbol);

    const mcp_plugin_descriptor* descriptor = nullptr;

    try {
        descriptor = query();
    } catch (...) {
        log_message(
            MCP_LOG_ERROR,
            "",
            "Exception crossed plugin query boundary: " + path
        );

        dlclose(record->library_handle);
        record->library_handle = nullptr;

        return false;
    }

    if (descriptor == nullptr) {
        log_message(
            MCP_LOG_ERROR,
            "",
            "Plugin returned a null descriptor: " + path
        );

        dlclose(record->library_handle);
        record->library_handle = nullptr;

        return false;
    }

    if (descriptor->abi_version != MCP_PLUGIN_ABI_VERSION ||
        descriptor->struct_size < sizeof(mcp_plugin_descriptor)) {

        log_message(
            MCP_LOG_ERROR,
            "",
            "Incompatible plugin ABI: " + path
        );

        dlclose(record->library_handle);
        record->library_handle = nullptr;

        return false;
    }

    if (!valid_string(descriptor->plugin_id) ||
        !valid_string(descriptor->plugin_version) ||
        descriptor->initialize == nullptr ||
        descriptor->shutdown == nullptr ||
        descriptor->list_tools == nullptr ||
        descriptor->execute_tool == nullptr) {

        log_message(
            MCP_LOG_ERROR,
            "",
            "Invalid plugin descriptor: " + path
        );

        dlclose(record->library_handle);
        record->library_handle = nullptr;

        return false;
    }

    record->descriptor = descriptor;
    record->id = descriptor->plugin_id;

    for (const auto& existing : plugins_) {
        if (existing->id == record->id) {
            log_message(
                MCP_LOG_ERROR,
                record->id,
                "Duplicate plugin identifier"
            );

            dlclose(record->library_handle);
            record->library_handle = nullptr;

            return false;
        }
    }

    log_message(
        MCP_LOG_INFO,
        record->id,
        "Initializing plugin version " +
            std::string(descriptor->plugin_version)
    );

    try {
        record->host_context =
            std::make_shared<AsyncState::HostContext>();

        record->host_context->state = async_state_.get();
        record->host_context->plugin_id = record->id;

        record->host_api.abi_version = MCP_PLUGIN_ABI_VERSION;
        record->host_api.struct_size = sizeof(mcp_host_api);
        record->host_api.host_context = record->host_context.get();
        record->host_api.log = host_log;
        record->host_api.get_extension = host_get_extension;
    } catch (...) {
        log_message(
            MCP_LOG_ERROR,
            record->id,
            "Failed to allocate plugin host context"
        );

        dlclose(record->library_handle);
        record->library_handle = nullptr;
        return false;
    }

    mcp_plugin_status init_status = MCP_PLUGIN_ERROR;

    try {
        init_status = descriptor->initialize(
            &record->host_api,
            configuration_json.c_str(),
            &record->plugin_context
        );
    } catch (...) {
        log_message(
            MCP_LOG_ERROR,
            record->id,
            "Exception crossed plugin initialize boundary"
        );

        async_state_->deactivate_plugin(*record->host_context);

        dlclose(record->library_handle);
        record->library_handle = nullptr;

        return false;
    }

    if (init_status != MCP_PLUGIN_OK) {
        log_message(
            MCP_LOG_ERROR,
            record->id,
            std::string("Initialization failed: ") +
                status_name(init_status)
        );

        async_state_->deactivate_plugin(*record->host_context);

        dlclose(record->library_handle);
        record->library_handle = nullptr;

        return false;
    }

    record->initialized = true;

    const mcp_tool_definition* definitions = nullptr;
    std::size_t definition_count = 0;

    mcp_plugin_status list_status = MCP_PLUGIN_ERROR;

    try {
        list_status = descriptor->list_tools(
            record->plugin_context,
            &definitions,
            &definition_count
        );
    } catch (...) {
        log_message(
            MCP_LOG_ERROR,
            record->id,
            "Exception crossed plugin list_tools boundary"
        );

        async_state_->deactivate_plugin(*record->host_context);
        descriptor->shutdown(record->plugin_context);
        record->initialized = false;

        dlclose(record->library_handle);
        record->library_handle = nullptr;

        return false;
    }

    if (list_status != MCP_PLUGIN_OK ||
        definition_count > MAX_PLUGIN_TOOLS ||
        (definition_count > 0 && definitions == nullptr)) {

        log_message(
            MCP_LOG_ERROR,
            record->id,
            "Invalid tool registry returned by plugin"
        );

        async_state_->deactivate_plugin(*record->host_context);
        descriptor->shutdown(record->plugin_context);
        record->initialized = false;

        dlclose(record->library_handle);
        record->library_handle = nullptr;

        return false;
    }

    std::vector<RegisteredTool> pending_tools;
    std::set<std::string> local_names;

    bool valid = true;

    for (std::size_t i = 0; i < definition_count; ++i) {
        const auto& definition = definitions[i];

        if (!valid_string(definition.name) ||
            !valid_string(definition.input_schema_json)) {

            valid = false;
            break;
        }

        const std::string tool_name = definition.name;

        if (!local_names.insert(tool_name).second) {
            valid = false;
            break;
        }

        for (const auto& registered : tools_) {
            if (registered.name == tool_name) {
                valid = false;
                break;
            }
        }

        if (!valid) {
            break;
        }

        if (definition.required_capabilities_count == 0 ||
            definition.required_capabilities_count >
                MAX_PLUGIN_CAPABILITIES ||
            definition.required_capabilities == nullptr) {

            valid = false;
            break;
        }

        RegisteredTool tool;

        tool.plugin_id = record->id;
        tool.name = tool_name;

        if (definition.description != nullptr) {
            tool.description = definition.description;
        }

        tool.input_schema_json = definition.input_schema_json;

        for (std::size_t j = 0;
             j < definition.required_capabilities_count;
             ++j) {

            const char* capability =
                definition.required_capabilities[j];

            if (!valid_string(capability)) {
                valid = false;
                break;
            }

            const std::string capability_name(capability);

            if (std::find(
                    tool.required_capabilities.begin(),
                    tool.required_capabilities.end(),
                    capability_name) !=
                tool.required_capabilities.end()) {

                valid = false;
                break;
            }

            tool.required_capabilities.emplace_back(
                capability_name
            );
        }

        if (!valid) {
            break;
        }

        pending_tools.push_back(std::move(tool));
    }

    if (!valid) {
        log_message(
            MCP_LOG_ERROR,
            record->id,
            "Plugin declared invalid or duplicate tools"
        );

        async_state_->deactivate_plugin(*record->host_context);
        descriptor->shutdown(record->plugin_context);
        record->initialized = false;

        dlclose(record->library_handle);
        record->library_handle = nullptr;

        return false;
    }

    tools_.insert(
        tools_.end(),
        pending_tools.begin(),
        pending_tools.end()
    );

    log_message(
        MCP_LOG_INFO,
        record->id,
        "Registered " +
            std::to_string(pending_tools.size()) +
            " tools"
    );

    plugins_.push_back(std::move(record));

    PluginRecord& loaded = *plugins_.back();

    if (!async_state_->activate_plugin(*loaded.host_context)) {
        log_message(
            MCP_LOG_ERROR,
            loaded.id,
            "Failed to activate plugin message receivers"
        );

        async_state_->deactivate_plugin(*loaded.host_context);

        tools_.erase(
            std::remove_if(
                tools_.begin(),
                tools_.end(),
                [&loaded](const RegisteredTool& tool) {
                    return tool.plugin_id == loaded.id;
                }
            ),
            tools_.end()
        );

        if (loaded.initialized && loaded.descriptor != nullptr) {
            try {
                loaded.descriptor->shutdown(loaded.plugin_context);
            } catch (...) {
                log_message(
                    MCP_LOG_ERROR,
                    loaded.id,
                    "Exception during rollback shutdown"
                );
            }

            loaded.initialized = false;
        }

        if (loaded.library_handle != nullptr) {
            dlclose(loaded.library_handle);
            loaded.library_handle = nullptr;
        }

        plugins_.pop_back();
        return false;
    }

    return true;
}

bool PluginManager::execute_tool(
    const std::string& tool_name,
    const std::string& arguments_json,
    std::string& result,
    std::string& error
)
{
    result.clear();
    error.clear();

    for (const auto& plugin : plugins_) {
        for (const auto& tool : tools_) {
            if (tool.plugin_id != plugin->id ||
                tool.name != tool_name) {
                continue;
            }

            std::size_t required_size = 0;

            mcp_plugin_status status = MCP_PLUGIN_ERROR;

            try {
                status = plugin->descriptor->execute_tool(
                    plugin->plugin_context,
                    tool_name.c_str(),
                    arguments_json.c_str(),
                    nullptr,
                    &required_size
                );
            } catch (...) {
                error = "Exception crossed plugin execution boundary";
                return false;
            }

            if (status != MCP_PLUGIN_BUFFER_TOO_SMALL &&
                status != MCP_PLUGIN_OK) {

                error = std::string("Plugin execution failed: ") +
                    status_name(status);

                return false;
            }

            if (required_size == 0 ||
                required_size > MAX_RESULT_SIZE) {

                error = "Plugin returned an invalid result size";
                return false;
            }

            std::vector<char> buffer(required_size, '\0');

            std::size_t capacity = buffer.size();

            try {
                status = plugin->descriptor->execute_tool(
                    plugin->plugin_context,
                    tool_name.c_str(),
                    arguments_json.c_str(),
                    buffer.data(),
                    &capacity
                );
            } catch (...) {
                error = "Exception crossed plugin execution boundary";
                return false;
            }

            if (status != MCP_PLUGIN_OK) {
                error = std::string("Plugin execution failed: ") +
                    status_name(status);

                return false;
            }

            if (capacity == 0 ||
                capacity > buffer.size() ||
                buffer[capacity - 1] != '\0') {

                error = "Plugin returned an invalid result buffer";
                return false;
            }

            result.assign(buffer.data(), capacity - 1);

            return true;
        }
    }

    error = "Tool not found: " + tool_name;

    return false;
}

const std::vector<RegisteredTool>& PluginManager::tools() const
{
    return tools_;
}

std::size_t PluginManager::plugin_count() const
{
    return plugins_.size();
}

std::size_t PluginManager::tool_count() const
{
    return tools_.size();
}

void PluginManager::set_message_handler(
    PluginMessageHandler handler
)
{
    if (!async_state_) {
        return;
    }

    AsyncState& state = *async_state_;

    // Handler lifecycle operations are not allowed from
    // inside the Core message callback.
    if (AsyncState::executing_core_handler == &state) {
        return;
    }

    {
        std::unique_lock<std::mutex> lock(state.mutex);

        state.core_callbacks_drained.wait(
            lock,
            [&state] {
                return !state.core_handler_draining;
            }
        );

        state.message_handler = std::move(handler);
    }

    state.available.notify_all();
}

void PluginManager::clear_message_handler()
{
    if (!async_state_) {
        return;
    }

    AsyncState& state = *async_state_;

    // Do not modify the handler from inside its own callback.
    if (AsyncState::executing_core_handler == &state) {
        return;
    }

    std::unique_lock<std::mutex> lock(state.mutex);

    state.core_callbacks_drained.wait(
        lock,
        [&state] {
            return !state.core_handler_draining;
        }
    );

    state.core_handler_draining = true;
    state.message_handler = nullptr;

    state.core_callbacks_drained.wait(
        lock,
        [&state] {
            return state.active_core_callbacks == 0;
        }
    );

    state.core_handler_draining = false;

    lock.unlock();

    state.core_callbacks_drained.notify_all();
    state.available.notify_all();
}

bool PluginManager::send_plugin_message(
    const std::string& plugin_id,
    const std::uint8_t* data,
    std::size_t size,
    std::string& error
)
{
    error.clear();

    if (!async_state_) {
        error = "Plugin message channel is unavailable";
        return false;
    }

    const mcp_plugin_status status =
        async_state_->enqueue_for_plugin(plugin_id, data, size);

    if (status != MCP_PLUGIN_OK) {
        error = std::string("Plugin message enqueue failed: ") +
            status_name(status);
        return false;
    }

    return true;
}

void PluginManager::shutdown()
{
    for (auto it = plugins_.rbegin();
         it != plugins_.rend();
         ++it) {

        PluginRecord& plugin = **it;

        if (plugin.host_context) {
            async_state_->deactivate_plugin(*plugin.host_context);
        }

        if (plugin.initialized &&
            plugin.descriptor != nullptr) {

            log_message(
                MCP_LOG_INFO,
                plugin.id,
                "Shutting down plugin"
            );

            try {
                plugin.descriptor->shutdown(
                    plugin.plugin_context
                );
            } catch (...) {
                log_message(
                    MCP_LOG_ERROR,
                    plugin.id,
                    "Exception during plugin shutdown"
                );
            }

            plugin.initialized = false;
        }

        if (plugin.library_handle != nullptr) {
            dlclose(plugin.library_handle);
            plugin.library_handle = nullptr;
        }
    }

    plugins_.clear();
    tools_.clear();
}

} // namespace mcp
