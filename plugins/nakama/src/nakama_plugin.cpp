#include "mcp/plugin_abi.h"
#include "nakama/websocket_transport.hpp"
#include "nakama/receive_dispatcher.hpp"
#include "nakama/http_auth.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <deque>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <json-c/json.h>
#include <json-c/json_tokener.h>
#include <curl/curl.h>
#include <climits>

namespace {

enum class ConfigState {
    MISSING,
    VALID,
    INVALID
};

struct NakamaConfig {
    std::string server_url;
    std::string server_key;
    std::string device_id;
    long connect_timeout_ms = 10000;
    bool verify_tls = true;
};

constexpr size_t MAX_CONFIG_SIZE = 16 * 1024;
constexpr const char* CONFIG_DIRECTORY = "mcp";
constexpr const char* CONFIG_FILENAME = "nakama.json";

bool exact_json_string(
    json_object* object,
    const char* key,
    std::string& output,
    size_t maximum_length
)
{
    json_object* value = nullptr;

    if (!json_object_object_get_ex(object, key, &value) ||
        value == nullptr ||
        json_object_get_type(value) != json_type_string) {
        return false;
    }

    const char* text = json_object_get_string(value);
    const int length = json_object_get_string_len(value);

    if (text == nullptr || length <= 0 ||
        static_cast<size_t>(length) > maximum_length ||
        std::strlen(text) != static_cast<size_t>(length)) {
        return false;
    }

    output.assign(text, static_cast<size_t>(length));
    return true;
}

bool has_duplicate_top_level_keys(const std::string& input)
{
    std::vector<std::string> keys;
    size_t i = 0;
    unsigned object_depth = 0;

    while (i < input.size()) {
        const char c = input[i];

        if (c == '"') {
            const size_t key_start = i + 1;
            size_t j = key_start;

            while (j < input.size()) {
                if (input[j] == '\\') {
                    j += 2;
                    continue;
                }

                if (input[j] == '"') {
                    break;
                }

                ++j;
            }

            if (j >= input.size()) {
                return true;
            }

            if (object_depth == 1) {
                size_t next = j + 1;

                while (next < input.size() &&
                       std::isspace(
                           static_cast<unsigned char>(input[next])
                       )) {
                    ++next;
                }

                if (next < input.size() && input[next] == ':') {
                    const std::string key =
                        input.substr(key_start, j - key_start);

                    // Configuration property names must be literal ASCII.
                    if (key.find('\\') != std::string::npos) {
                        return true;
                    }

                    for (const std::string& existing : keys) {
                        if (existing == key) {
                            return true;
                        }
                    }

                    keys.push_back(key);
                }
            }

            i = j + 1;
            continue;
        }

        if (c == '{') {
            ++object_depth;
        } else if (c == '}') {
            if (object_depth == 0) {
                return true;
            }
            --object_depth;
        }

        ++i;
    }

    return false;
}

bool validate_server_url(
    const std::string& input,
    bool verify_tls
)
{
    const bool is_wss =
    input.compare(0, 6, "wss://") == 0;

    const bool is_ws =
    input.compare(0, 5, "ws://") == 0;

    if ((verify_tls && !is_wss) ||
        (!verify_tls && !is_ws)) {
        return false;
        }

    const size_t authority_start = is_wss ? 6 : 5;
    const size_t authority_end = input.find_first_of("/?#", authority_start);

    const size_t end = authority_end == std::string::npos
        ? input.size()
        : authority_end;

    const std::string authority =
        input.substr(authority_start, end - authority_start);

    if (authority.empty() ||
        authority.find('@') != std::string::npos ||
        authority.find('\\') != std::string::npos ||
        authority.find('%') != std::string::npos ||
        authority.back() == ':') {
        return false;
    }

    CURLU* raw_url = curl_url();
    if (raw_url == nullptr) {
        return false;
    }

    struct UrlCleanup {
        CURLU* value;
        ~UrlCleanup() { curl_url_cleanup(value); }
    } url_cleanup{raw_url};

    if (curl_url_set(
            raw_url,
            CURLUPART_URL,
            input.c_str(),
            CURLU_DISALLOW_USER
        ) != CURLUE_OK) {
        return false;
    }

    auto get_part = [raw_url](
        CURLUPart part,
        std::string& output,
        unsigned int flags = 0
    ) -> CURLUcode {
        char* value = nullptr;
        const CURLUcode result =
            curl_url_get(raw_url, part, &value, flags);

        if (result == CURLUE_OK && value != nullptr) {
            output.assign(value);
        }

        curl_free(value);
        return result;
    };

    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
    std::string query;
    std::string fragment;

    if (get_part(CURLUPART_SCHEME, scheme) != CURLUE_OK ||
        scheme != (verify_tls ? "wss" : "ws") ||
        get_part(CURLUPART_HOST, host) != CURLUE_OK ||
        host.empty()) {
        return false;
    }

    const CURLUcode port_result =
        get_part(CURLUPART_PORT, port, CURLU_NO_DEFAULT_PORT);

    if (port_result == CURLUE_OK) {
        if (port.empty()) {
            return false;
        }

        unsigned int port_number = 0;
        for (const unsigned char c : port) {
            if (c < '0' || c > '9') {
                return false;
            }

            const unsigned int digit =
                static_cast<unsigned int>(c - '0');

            if (port_number > (65535u - digit) / 10u) {
                return false;
            }

            port_number = port_number * 10u + digit;
        }

        if (port_number == 0) {
            return false;
        }
    } else if (port_result != CURLUE_NO_PORT) {
        return false;
    }

    if (get_part(CURLUPART_PATH, path) != CURLUE_OK ||
        (path != "" && path != "/")) {
        return false;
    }

    const CURLUcode query_result =
        get_part(CURLUPART_QUERY, query);
    if (query_result != CURLUE_NO_QUERY) {
        return false;
    }

    const CURLUcode fragment_result =
        get_part(CURLUPART_FRAGMENT, fragment);
    if (fragment_result != CURLUE_NO_FRAGMENT) {
        return false;
    }

    return true;
}

bool parse_config_json(
    const std::string& input,
    NakamaConfig& output
)
{
    bool verify_tls_enabled = false;

    if (input.empty() || input.size() > MAX_CONFIG_SIZE ||
        input.size() > static_cast<size_t>(INT32_MAX)) {
        return false;
    }

    if (has_duplicate_top_level_keys(input)) {
        return false;
    }

    json_tokener* tokener = json_tokener_new();
    if (tokener == nullptr) {
        return false;
    }

    json_tokener_set_flags(
        tokener,
        JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8
    );

    json_object* root = json_tokener_parse_ex(
        tokener,
        input.data(),
        static_cast<int>(input.size())
    );

    const bool parse_ok =
        json_tokener_get_error(tokener) == json_tokener_success &&
        root != nullptr;

    const size_t parse_end = json_tokener_get_parse_end(tokener);
    json_tokener_free(tokener);

    if (!parse_ok) {
        if (root != nullptr) {
            json_object_put(root);
        }
        return false;
    }

    bool valid = json_object_get_type(root) == json_type_object &&
                 json_object_object_length(root) == 5;

    static const char* const allowed_keys[] = {
        "server_url",
        "server_key",
        "device_id",
        "connect_timeout_ms",
        "verify_tls"
    };

    if (valid) {
        json_object_object_foreach(root, key, value) {
            (void)value;
            bool found = false;
            for (const char* allowed : allowed_keys) {
                if (std::strcmp(key, allowed) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                valid = false;
                break;
            }
        }
    }

    std::string server_url;
    std::string server_key;
    std::string device_id;

    if (valid) {
        valid = exact_json_string(
                    root, "server_url", server_url, 2048
                ) &&
                exact_json_string(
                    root, "server_key", server_key, 4096
                ) &&
                exact_json_string(
                    root, "device_id", device_id, 256
                );
    }

    json_object* timeout = nullptr;
    json_object* verify_tls = nullptr;

    if (valid) {
        valid =
            json_object_object_get_ex(
                root, "connect_timeout_ms", &timeout
            ) &&
            timeout != nullptr &&
            json_object_get_type(timeout) == json_type_int &&
            json_object_get_int64(timeout) >= 1000 &&
            json_object_get_int64(timeout) <= 60000 &&
            json_object_object_get_ex(
                root, "verify_tls", &verify_tls
            ) &&
            verify_tls != nullptr &&
            json_object_get_type(verify_tls) == json_type_boolean;
    }

    if (valid) {
        verify_tls_enabled =
            json_object_get_boolean(verify_tls) == 1;

        valid = validate_server_url(
            server_url,
            verify_tls_enabled
        );
    }

    // Permit only trailing JSON whitespace after the parsed object.
    if (valid) {
        for (size_t i = parse_end; i < input.size(); ++i) {
            const unsigned char c =
                static_cast<unsigned char>(input[i]);

            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                valid = false;
                break;
            }
        }
    }

    if (valid) {
        NakamaConfig candidate;

        candidate.server_url = server_url;
        candidate.server_key = server_key;
        candidate.device_id = device_id;

        candidate.connect_timeout_ms =
            static_cast<long>(json_object_get_int64(timeout));

        candidate.verify_tls = verify_tls_enabled;

        output = std::move(candidate);
    }

    json_object_put(root);
    return valid;
}

[[maybe_unused]] bool validate_config_json(const std::string& input)
{
    NakamaConfig config;
    return parse_config_json(input, config);
}

bool same_file_snapshot(
    const struct stat& initial,
    const struct stat& final
)
{
    return
        final.st_dev == initial.st_dev &&
        final.st_ino == initial.st_ino &&
        final.st_uid == initial.st_uid &&
        final.st_mode == initial.st_mode &&
        final.st_nlink == initial.st_nlink &&
        final.st_size == initial.st_size &&
        final.st_mtim.tv_sec == initial.st_mtim.tv_sec &&
        final.st_mtim.tv_nsec == initial.st_mtim.tv_nsec &&
        final.st_ctim.tv_sec == initial.st_ctim.tv_sec &&
        final.st_ctim.tv_nsec == initial.st_ctim.tv_nsec;
}

ConfigState load_config_state_at(
    int db_fd,
    uid_t effective_uid,
    NakamaConfig* output = nullptr
)
{
    int directory_fd = openat(
        db_fd,
        CONFIG_DIRECTORY,
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC
    );

    const int directory_error = errno;

    if (directory_fd < 0) {
        return directory_error == ENOENT
            ? ConfigState::MISSING
            : ConfigState::INVALID;
    }

    struct stat directory_stat {};
    if (fstat(directory_fd, &directory_stat) != 0 ||
        !S_ISDIR(directory_stat.st_mode) ||
        directory_stat.st_uid != effective_uid ||
        (directory_stat.st_mode & 07777) != 0700) {
        close(directory_fd);
        return ConfigState::INVALID;
    }

    int file_fd = openat(
        directory_fd,
        CONFIG_FILENAME,
        O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC
    );

    const int file_error = errno;
    close(directory_fd);

    if (file_fd < 0) {
        return file_error == ENOENT
            ? ConfigState::MISSING
            : ConfigState::INVALID;
    }

    struct stat file_stat {};
    if (fstat(file_fd, &file_stat) != 0 ||
        !S_ISREG(file_stat.st_mode) ||
        file_stat.st_uid != effective_uid ||
        (file_stat.st_mode & 07777) != 0600 ||
        file_stat.st_size <= 0 ||
        file_stat.st_nlink != 1 ||
        static_cast<uint64_t>(file_stat.st_size) > MAX_CONFIG_SIZE) {
        close(file_fd);
        return ConfigState::INVALID;
    }

    std::string contents;
    contents.reserve(static_cast<size_t>(file_stat.st_size));

    char buffer[2048];
    bool read_ok = true;

    for (;;) {
        const ssize_t count = read(file_fd, buffer, sizeof(buffer));

        if (count > 0) {
            if (contents.size() + static_cast<size_t>(count) >
                MAX_CONFIG_SIZE) {
                read_ok = false;
                break;
            }
            contents.append(buffer, static_cast<size_t>(count));
            continue;
        }

        if (count == 0) {
            break;
        }

        if (errno == EINTR) {
            continue;
        }

        read_ok = false;
        break;
    }

    struct stat final_stat {};
    const bool final_stat_ok = fstat(file_fd, &final_stat) == 0;

    close(file_fd);

    if (!read_ok ||
        !final_stat_ok ||
        contents.size() != static_cast<size_t>(file_stat.st_size) ||
        !same_file_snapshot(file_stat, final_stat)) {
        return ConfigState::INVALID;
    }

    NakamaConfig parsed_config;

    if (!parse_config_json(contents, parsed_config)) {
        return ConfigState::INVALID;
    }

    if (output != nullptr) {
        *output = std::move(parsed_config);
    }

    return ConfigState::VALID;
}

ConfigState load_config_state(NakamaConfig* output = nullptr)
{
    const uid_t effective_uid = geteuid();

    int db_fd = open(
        "/var/db",
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC
    );

    if (db_fd < 0) {
        return ConfigState::INVALID;
    }

    struct stat db_stat {};
    if (fstat(db_fd, &db_stat) != 0 ||
        !S_ISDIR(db_stat.st_mode) ||
        db_stat.st_uid != 0 ||
        (db_stat.st_mode & 0022) != 0) {
        close(db_fd);
        return ConfigState::INVALID;
    }

    const ConfigState result =
    load_config_state_at(db_fd, effective_uid, output);

    close(db_fd);
    return result;
}

constexpr const char* PLUGIN_ID = "nakama";

struct NakamaPluginContext {
    const mcp_host_api* host;
    NakamaConfig config;
    ConfigState config_state;

    // Connection lifecycle: connect/disconnect serialization.
    std::mutex lifecycle_mutex;

    // Protects transport ownership and pointer publication.
    std::mutex transport_mutex;

    // Temporary: used by the legacy receive path until dispatcher integration.

    // Protects messages waiting for future Agent Bus handling.
    std::mutex messages_mutex;

    // Protects the last reported transport or authentication error.
    std::mutex error_mutex;

    std::shared_ptr<nakama::WebSocketTransport> transport;
    std::shared_ptr<nakama::ReceiveDispatcher> dispatcher;

    std::string jwt;
    std::string last_error;
    std::deque<nakama::WebSocketMessage> pending_messages;
};

std::shared_ptr<nakama::WebSocketTransport> get_transport(
    NakamaPluginContext* context
)
{
    if (context == nullptr) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(context->transport_mutex);
    return context->transport;
}

void set_transport(
    NakamaPluginContext* context,
    std::shared_ptr<nakama::WebSocketTransport> transport
)
{
    if (context == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(context->transport_mutex);
    context->transport = transport;
}

std::string get_last_error(NakamaPluginContext* context)
{
    if (context == nullptr) {
        return {};

    }

    std::lock_guard<std::mutex> lock(context->error_mutex);
    return context->last_error;
}

void set_last_error(
    NakamaPluginContext* context,
    const std::string& error
)
{
    if (context == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(context->error_mutex);
    context->last_error = error;
}

void clear_last_error(NakamaPluginContext* context)
{
    if (context == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(context->error_mutex);
    context->last_error.clear();
}

// Caller must serialize lifecycle operations through lifecycle_mutex.
void stop_receive_dispatcher(NakamaPluginContext* context)
{
    if (context == nullptr || !context->dispatcher) {
        return;
    }

    auto dispatcher = std::move(context->dispatcher);
    dispatcher->stop();
}

bool start_receive_dispatcher(
    NakamaPluginContext* context,
    const std::shared_ptr<nakama::WebSocketTransport>& transport,
    std::string& error
)
{
    if (context == nullptr || !transport) {
        error = "Invalid context or transport for receive dispatcher";
        return false;
    }

    if (context->dispatcher) {
        return true;
    }

    std::shared_ptr<nakama::ReceiveDispatcher> dispatcher;

    try {
        dispatcher = std::make_shared<nakama::ReceiveDispatcher>(
            transport,
            [context](const nakama::WebSocketMessage& message) {
                std::lock_guard<std::mutex> lock(context->messages_mutex);

                if (context->pending_messages.size() < 256) {
                    context->pending_messages.push_back(message);
                }
            },
            [context](const std::string& dispatcher_error) {
                set_last_error(context, dispatcher_error);
            }
        );

        if (!dispatcher->start()) {
            dispatcher->stop();
            error = "Could not start Nakama receive dispatcher";
            return false;
        }

        context->dispatcher = std::move(dispatcher);
        return true;
    } catch (...) {
        if (dispatcher) {
            dispatcher->stop();
        }

        error = "Exception while creating Nakama receive dispatcher";
        return false;
    }
}

static const char EMPTY_OBJECT_SCHEMA[] =
    "{\"type\":\"object\",\"properties\":{},"
    "\"additionalProperties\":false}";

static const char* const NAKAMA_READ_CAPABILITIES[] = {
    "nakama.read"
};

static const char* const NAKAMA_CONTROL_CAPABILITIES[] = {
    "nakama.control"
};

static const mcp_tool_definition TOOLS[] = {
    {
        "nakama_connect",
        "Connect and authenticate with the Nakama server",
        EMPTY_OBJECT_SCHEMA,
        NAKAMA_CONTROL_CAPABILITIES,
        sizeof(NAKAMA_CONTROL_CAPABILITIES) /
        sizeof(NAKAMA_CONTROL_CAPABILITIES[0])
    },
    {
        "nakama_status",
        "Get Nakama plugin and WebSocket connection status",
        EMPTY_OBJECT_SCHEMA,
        NAKAMA_READ_CAPABILITIES,
        sizeof(NAKAMA_READ_CAPABILITIES) /
        sizeof(NAKAMA_READ_CAPABILITIES[0])
    },
    {
        "nakama_ping",
        "Send an application-level ping to the Nakama server",
        EMPTY_OBJECT_SCHEMA,
        NAKAMA_READ_CAPABILITIES,
        sizeof(NAKAMA_READ_CAPABILITIES) /
        sizeof(NAKAMA_READ_CAPABILITIES[0])
    },
    {
        "nakama_disconnect",
        "Disconnect the Nakama WebSocket transport",
        EMPTY_OBJECT_SCHEMA,
        NAKAMA_CONTROL_CAPABILITIES,
        sizeof(NAKAMA_CONTROL_CAPABILITIES) /
        sizeof(NAKAMA_CONTROL_CAPABILITIES[0])
    }
};

void log_message(
    NakamaPluginContext* context,
    mcp_log_level level,
    const char* message
)
{
    if (context == nullptr ||
        context->host == nullptr ||
        context->host->log == nullptr) {
        return;
    }

    context->host->log(
        context->host->host_context,
        level,
        PLUGIN_ID,
        message
    );
}

bool valid_empty_arguments(const char* input)
{
    if (input == nullptr) {
        return false;
    }

    const char* current = input;

    while (*current != '\0' &&
           std::isspace(static_cast<unsigned char>(*current))) {
        ++current;
    }

    if (*current != '{') {
        return false;
    }

    ++current;

    while (*current != '\0' &&
           std::isspace(static_cast<unsigned char>(*current))) {
        ++current;
    }

    if (*current != '}') {
        return false;
    }

    ++current;

    while (*current != '\0' &&
           std::isspace(static_cast<unsigned char>(*current))) {
        ++current;
    }

    return *current == '\0';
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

    const size_t required_size = result.size() + 1;

    if (result_buffer == nullptr ||
        *result_size < required_size) {
        *result_size = required_size;
        return MCP_PLUGIN_BUFFER_TOO_SMALL;
    }

    std::memcpy(
        result_buffer,
        result.c_str(),
        required_size
    );

    *result_size = required_size;
    return MCP_PLUGIN_OK;
}

const char* transport_state_name(nakama::TransportState state)
{
    switch (state) {
        case nakama::TransportState::STOPPED:
            return "STOPPED";
        case nakama::TransportState::CONNECTING:
            return "CONNECTING";
        case nakama::TransportState::CONNECTED:
            return "CONNECTED";
        case nakama::TransportState::CLOSING:
            return "CLOSING";
        case nakama::TransportState::ERROR:
            return "ERROR";
    }

    return "UNKNOWN";
}




mcp_plugin_status initialize(
    const mcp_host_api* host,
    const char* configuration_json,
    void** plugin_context
)
{
    (void)configuration_json;

    if (plugin_context == nullptr ||
        host == nullptr ||
        host->abi_version != MCP_PLUGIN_ABI_VERSION ||
        host->struct_size < sizeof(mcp_host_api)) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
        }

        *plugin_context = nullptr;

    auto* context = new (std::nothrow) NakamaPluginContext{};

    if (context == nullptr) {
        return MCP_PLUGIN_ERROR;
    }

    context->host = host;
    context->config = NakamaConfig{};
    context->config_state = ConfigState::MISSING;

    context->config_state = load_config_state(&context->config);

    *plugin_context = context;

    log_message(
        context,
        MCP_LOG_INFO,
        "Nakama plugin initialized; configuration state evaluated"
    );

    return MCP_PLUGIN_OK;
}

void shutdown(void* plugin_context)
{
    auto* context =
        static_cast<NakamaPluginContext*>(plugin_context);

    log_message(
        context,
        MCP_LOG_INFO,
        "Nakama plugin shutting down"
    );

    if (context != nullptr) {
        std::lock_guard<std::mutex> lifecycle_lock(
            context->lifecycle_mutex
        );

        stop_receive_dispatcher(context);

        auto transport = get_transport(context);

        if (transport != nullptr) {
            transport->stop();
            set_transport(context, nullptr);
        }
    }

    delete context;
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

        if (!valid_empty_arguments(arguments_json)) {
            return MCP_PLUGIN_INVALID_ARGUMENT;
        }

        auto* context =
        static_cast<NakamaPluginContext*>(plugin_context);

        try {
            const std::string name(tool_name);

            const bool known_tool =
            name == "nakama_connect" ||
            name == "nakama_status" ||
            name == "nakama_ping" ||
            name == "nakama_disconnect";

            if (!known_tool) {
                return MCP_PLUGIN_NOT_FOUND;
            }

            // First ABI call: report the required buffer size.
            // Never perform network operations during this call.
            constexpr size_t RESULT_CAPACITY = 8192;

            if (result_buffer == nullptr ||
                *result_size < RESULT_CAPACITY) {
                *result_size = RESULT_CAPACITY;
            return MCP_PLUGIN_BUFFER_TOO_SMALL;
                }

                bool response_build_failed = false;

                auto emit = [&](
                    json_object* root
                ) -> mcp_plugin_status {
                    if (root == nullptr) {
                        return MCP_PLUGIN_ERROR;
                    }

                    if (response_build_failed) {
                        json_object_put(root);
                        return MCP_PLUGIN_ERROR;
                    }

                    const char* serialized =
                    json_object_to_json_string_ext(
                        root,
                        JSON_C_TO_STRING_PLAIN
                    );

                    if (serialized == nullptr) {
                        json_object_put(root);
                        return MCP_PLUGIN_ERROR;
                    }

                    const std::string result(serialized);
                    json_object_put(root);

                    return write_result(
                        result,
                        result_buffer,
                        result_size
                    );
                };

                auto make_response = []() -> json_object* {
                    return json_object_new_object();
                };

                auto add_string = [&](
                    json_object* object,
                    const char* key,
                    const std::string& value
                ) -> bool {
                    if (object == nullptr ||
                        key == nullptr ||
                        value.size() >
                        static_cast<std::size_t>(INT_MAX)) {
                        response_build_failed = true;
                    return false;
                        }

                        json_object* json_value =
                        json_object_new_string_len(
                            value.data(),
                                                   static_cast<int>(value.size())
                        );

                        if (json_value == nullptr) {
                            response_build_failed = true;
                            return false;
                        }

                        if (json_object_object_add(
                            object,
                            key,
                            json_value
                        ) != 0) {
                            json_object_put(json_value);
                            response_build_failed = true;
                            return false;
                        }

                        return true;
                };

                auto add_boolean = [&](
                    json_object* object,
                    const char* key,
                    bool value
                ) -> bool {
                    if (object == nullptr || key == nullptr) {
                        response_build_failed = true;
                        return false;
                    }

                    json_object* json_value =
                    json_object_new_boolean(value);

                    if (json_value == nullptr) {
                        response_build_failed = true;
                        return false;
                    }

                    if (json_object_object_add(
                        object,
                        key,
                        json_value
                    ) != 0) {
                        json_object_put(json_value);
                        response_build_failed = true;
                        return false;
                    }

                    return true;
                };


                // STATUS uses independent state locks and does not block
                // connection lifecycle operations or ping/pong waits.
                if (name == "nakama_status") {

                    json_object* root = make_response();

                    if (root == nullptr) {
                        return MCP_PLUGIN_ERROR;
                    }

                    const bool configured =
                    context->config_state == ConfigState::VALID;

                    const bool configuration_error =
                    context->config_state == ConfigState::INVALID;

                    const char* config_name =
                    configured ? "VALID" :
                    configuration_error ? "INVALID" : "MISSING";

                    const auto transport = get_transport(context);

                    const nakama::TransportState state =
                    transport != nullptr
                    ? transport->state()
                    : nakama::TransportState::STOPPED;

                    const bool connected =
                    state == nakama::TransportState::CONNECTED;

                    add_string(root, "plugin", "nakama");
                    add_string(root, "configuration_state", config_name);
                    add_string(
                        root,
                        "transport_state",
                        transport_state_name(state)
                    );

                    add_boolean(root, "configured", configured);
                    add_boolean(root, "configuration_error", configuration_error);
                    add_boolean(root, "connected", connected);

                    const std::string last_error = get_last_error(context);

                    if (!last_error.empty()) {
                        add_string(root, "last_error", last_error);
                    }

                    return emit(root);
                }

                // -------------------------------------------------
                // NAKAMA CONNECT
                // -------------------------------------------------

                if (name == "nakama_connect") {
                    std::lock_guard<std::mutex> lifecycle_lock(
                        context->lifecycle_mutex
                    );

                    auto transport = get_transport(context);

                    if (context->config_state != ConfigState::VALID) {
                        set_last_error(
                            context,
                            "Nakama configuration is missing or invalid"
                        );

                json_object* root = make_response();

                if (root == nullptr) {
                    return MCP_PLUGIN_ERROR;
                }

                add_boolean(root, "connected", false);

                add_string(
                    root,
                    "error",
                    get_last_error(context)
                );

                return emit(root);
                    }

                    if (transport != nullptr &&
                        transport->is_connected()) {
                        std::string dispatcher_error;

                        if (!start_receive_dispatcher(
                            context,
                            transport,
                            dispatcher_error)) {
                            set_last_error(context, dispatcher_error);
                            stop_receive_dispatcher(context);
                            transport->stop();
                            set_transport(context, nullptr);
                            context->jwt.clear();

                            json_object* root = make_response();
                            if (root == nullptr) {
                                return MCP_PLUGIN_ERROR;
                            }

                            add_boolean(root, "connected", false);
                            add_string(root, "error", get_last_error(context));
                            return emit(root);
                        }

                        json_object* root = make_response();

                    if (root == nullptr) {
                        return MCP_PLUGIN_ERROR;
                    }

                    add_boolean(root, "connected", true);

                    add_boolean(root, "already_connected", true);

                    return emit(root);
                        }

                        if (transport != nullptr) {
                            stop_receive_dispatcher(context);
                            transport->stop();
                            set_transport(context, nullptr);
                            transport.reset();
                        } else {
                            stop_receive_dispatcher(context);
                        }

                        context->jwt.clear();

                        {
                            std::lock_guard<std::mutex> messages_lock(
                                context->messages_mutex
                            );
                            context->pending_messages.clear();
                        }

                        clear_last_error(context);

                        nakama::HttpAuthConfig auth_config;

                        auth_config.server_url = context->config.server_url;
                        auth_config.server_key = context->config.server_key;
                        auth_config.device_id = context->config.device_id;
                        auth_config.connect_timeout_ms =
                        context->config.connect_timeout_ms;
                        auth_config.verify_tls = context->config.verify_tls;

                        std::string token;
                        std::string auth_error;

                        if (!nakama::authenticate_device(
                            auth_config,
                            token,
                            auth_error)) {
                            set_last_error(
                                context,
                                auth_error.empty()
                                ? "Nakama authentication failed"
                                : auth_error
                            );

                        json_object* root = make_response();

                        if (root == nullptr) {
                            return MCP_PLUGIN_ERROR;
                        }

                        add_boolean(root, "connected", false);

                        add_string(root, "error", get_last_error(context));

                        return emit(root);
                            }

                            // Build the WebSocket URL with an escaped JWT.
                            CURL* curl = curl_easy_init();

                            if (curl == nullptr) {
                                set_last_error(
                                    context,
                                    "Could not initialize WebSocket URL encoder"
                                );

                        json_object* root = make_response();

                        if (root == nullptr) {
                            return MCP_PLUGIN_ERROR;
                        }

                        add_boolean(root, "connected", false);

                        add_string(root, "error", get_last_error(context));

                        return emit(root);
                            }

                            char* escaped_token = curl_easy_escape(
                                curl,
                                token.c_str(),
                                                                   static_cast<int>(token.size())
                            );

                            if (escaped_token == nullptr) {
                                curl_easy_cleanup(curl);

                                set_last_error(
                                    context,
                                    "Could not encode Nakama authentication token"
                                );

                        json_object* root = make_response();

                        if (root == nullptr) {
                            return MCP_PLUGIN_ERROR;
                        }

                        add_boolean(root, "connected", false);

                        add_string(root, "error", get_last_error(context));

                        return emit(root);
                            }

                            std::string websocket_url =
                            context->config.server_url +
                            "/ws?token=" + escaped_token +
                            "&format=json&lang=en";

                            curl_free(escaped_token);
                            curl_easy_cleanup(curl);

                            context->jwt = token;

                            nakama::WebSocketConfig ws_config;

                            ws_config.url = websocket_url;
                            ws_config.connect_timeout_ms =
                            context->config.connect_timeout_ms;
                            ws_config.io_poll_interval_ms = 100;
                            ws_config.verify_tls = context->config.verify_tls;

                            transport = std::make_shared<nakama::WebSocketTransport>(
                                ws_config
                            );

                            set_transport(context, transport);

                            if (!transport->start()) {

                                std::string error = transport->last_error();

                                if (error.empty()) {
                                    error = "Could not start Nakama WebSocket transport";
                                }

                                set_last_error(context, error);

                                transport->stop();
                                set_transport(context, nullptr);
                                transport.reset();
                                context->jwt.clear();

                                json_object* root = make_response();

                                if (root == nullptr) {
                                    return MCP_PLUGIN_ERROR;
                                }

                                add_boolean(root, "connected", false);

                                add_string(root, "error", get_last_error(context));

                                return emit(root);
                            }

                            const auto deadline =
                            std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(
                                context->config.connect_timeout_ms + 1000
                            );

                            nakama::TransportState state =
                            nakama::TransportState::CONNECTING;

                            while (std::chrono::steady_clock::now() < deadline) {
                                state = transport->state();

                                if (state == nakama::TransportState::CONNECTED ||
                                    state == nakama::TransportState::ERROR ||
                                    state == nakama::TransportState::STOPPED) {
                                    break;
                                    }

                                    std::this_thread::sleep_for(
                                        std::chrono::milliseconds(20)
                                    );
                            }

                            state = transport->state();

                            bool connected =
                            state == nakama::TransportState::CONNECTED;

                            if (!connected) {

                                std::string error = transport->last_error();

                                if (error.empty()) {
                                    error = "Nakama WebSocket connection timed out or stopped";
                                }

                                set_last_error(context, error);

                                transport->stop();
                                state = transport->state();
                                set_transport(context, nullptr);
                                transport.reset();
                                context->jwt.clear();
                            }

                            if (connected) {
                                std::string dispatcher_error;

                                if (!start_receive_dispatcher(
                                    context,
                                    transport,
                                    dispatcher_error)) {
                                    set_last_error(context, dispatcher_error);

                                    stop_receive_dispatcher(context);
                                    transport->stop();

                                    state = transport->state();
                                    set_transport(context, nullptr);
                                    transport.reset();
                                    context->jwt.clear();
                                    connected = false;
                                }
                            }

                            json_object* root = make_response();

                            if (root == nullptr) {
                                return MCP_PLUGIN_ERROR;
                            }

                            add_boolean(root, "connected", connected);

                            add_string(
                                root,
                                "transport_state",
                                transport_state_name(state)
                            );

                            if (!connected) {
                                add_string(root, "error", get_last_error(context));
                            }

                            return emit(root);
                }

                // -------------------------------------------------
                // NAKAMA PING
                // -------------------------------------------------

                if (name == "nakama_ping") {
                    std::shared_ptr<nakama::WebSocketTransport> transport;
                    std::shared_ptr<nakama::ReceiveDispatcher> dispatcher;

                    {
                        std::lock_guard<std::mutex> lifecycle_lock(
                            context->lifecycle_mutex
                        );

                        transport = get_transport(context);
                        dispatcher = context->dispatcher;
                    }

                    if (transport == nullptr ||
                        dispatcher == nullptr ||
                        !transport->is_connected()) {
                        json_object* root = make_response();

                        if (root == nullptr) {
                            return MCP_PLUGIN_ERROR;
                        }

                        add_boolean(root, "pong", false);
                        add_string(
                            root,
                            "error",
                            "Nakama WebSocket or receive dispatcher is not connected"
                        );

                        return emit(root);
                    }

                    static std::atomic<uint64_t> ping_counter{0};

                    const uint64_t sequence = ++ping_counter;
                    const auto timestamp =
                        std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count();

                    const std::string cid =
                        std::to_string(timestamp) + "-" +
                        std::to_string(sequence);

                    json_object* request = json_object_new_object();

                    if (request == nullptr) {
                        return MCP_PLUGIN_ERROR;
                    }

                    json_object* cid_value =
                        json_object_new_string_len(
                            cid.data(),
                            static_cast<int>(cid.size())
                        );

                    if (cid_value == nullptr ||
                        json_object_object_add(
                            request,
                            "cid",
                            cid_value
                        ) != 0) {
                        if (cid_value != nullptr) {
                            json_object_put(cid_value);
                        }
                        json_object_put(request);
                        return MCP_PLUGIN_ERROR;
                    }

                    json_object* ping_value = json_object_new_object();

                    if (ping_value == nullptr ||
                        json_object_object_add(
                            request,
                            "ping",
                            ping_value
                        ) != 0) {
                        if (ping_value != nullptr) {
                            json_object_put(ping_value);
                        }
                        json_object_put(request);
                        return MCP_PLUGIN_ERROR;
                    }

                    const char* serialized =
                        json_object_to_json_string_ext(
                            request,
                            JSON_C_TO_STRING_PLAIN
                        );

                    if (serialized == nullptr) {
                        json_object_put(request);
                        return MCP_PLUGIN_ERROR;
                    }

                    const std::string payload(serialized);
                    json_object_put(request);

                    if (!dispatcher->register_pending(cid)) {
                        json_object* root = make_response();

                        if (root == nullptr) {
                            return MCP_PLUGIN_ERROR;
                        }

                        add_boolean(root, "pong", false);
                        add_string(root, "error", "Could not register pending Nakama ping");

                        return emit(root);
                    }

                    if (!transport->send_text(payload)) {
                        dispatcher->cancel_pending(cid);

                        json_object* root = make_response();

                        if (root == nullptr) {
                            return MCP_PLUGIN_ERROR;
                        }

                        add_boolean(root, "pong", false);
                        add_string(root, "error", "Could not send Nakama ping");

                        return emit(root);
                    }

                    nakama::WebSocketMessage response;

                    const bool pong_received =
                        dispatcher->wait_for_response(
                            cid,
                            std::chrono::seconds(5),
                            response
                        ) &&
                        response.type == nakama::WebSocketMessageType::TEXT;

                    if (!pong_received) {
                        dispatcher->cancel_pending(cid);
                    }

                    json_object* root = make_response();

                    if (root == nullptr) {
                        return MCP_PLUGIN_ERROR;
                    }

                    add_boolean(root, "pong", pong_received);
                    add_string(root, "cid", cid);

                    if (!pong_received) {
                        const std::string last_error = get_last_error(context);

                        add_string(
                            root,
                            "error",
                            last_error.empty()
                                ? "Nakama ping timed out"
                                : last_error
                        );
                    }

                    return emit(root);
                }

                // -------------------------------------------------
                // NAKAMA DISCONNECT
                // -------------------------------------------------

                if (name == "nakama_disconnect") {
                    std::lock_guard<std::mutex> lifecycle_lock(
                        context->lifecycle_mutex
                    );
                    auto transport = get_transport(context);
                    const bool was_connected =
                    transport != nullptr &&
                    transport->is_connected();

                    stop_receive_dispatcher(context);

                    if (transport != nullptr) {
                        transport->stop();
                        set_transport(context, nullptr);
                        transport.reset();
                    }

                    context->jwt.clear();

                    {
                        std::lock_guard<std::mutex> messages_lock(
                            context->messages_mutex
                        );
                        context->pending_messages.clear();
                    }

                    clear_last_error(context);

                    json_object* root = make_response();

                    if (root == nullptr) {
                        return MCP_PLUGIN_ERROR;
                    }

                    add_boolean(root, "disconnected", true);

                    add_boolean(root, "was_connected", was_connected);

                    return emit(root);
                }

                return MCP_PLUGIN_NOT_FOUND;
        } catch (...) {
            return MCP_PLUGIN_ERROR;
        }
}


const mcp_plugin_descriptor DESCRIPTOR = {
    MCP_PLUGIN_ABI_VERSION,
    sizeof(mcp_plugin_descriptor),
    PLUGIN_ID,
    "0.1.0",
    "Native Nakama integration plugin for MCP Core",
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
