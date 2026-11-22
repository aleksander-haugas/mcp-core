#include "mcp/llama_client.hpp"

#include <curl/curl.h>
#include <json-c/json.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <limits>
#include <mutex>
#include <new>
#include <sstream>
#include <utility>

namespace mcp {
namespace {

std::once_flag curl_init_flag;
CURLcode curl_init_result = CURLE_FAILED_INIT;

void initialize_curl()
{
    std::call_once(curl_init_flag, [] {
        curl_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

bool valid_base_url(const std::string& value, std::string& error)
{
    initialize_curl();
    if (curl_init_result != CURLE_OK) {
        error = "libcurl global initialization failed";
        return false;
    }

    if (value.empty() || value.find('\0') != std::string::npos) {
        error = "base_url is empty or contains a NUL byte";
        return false;
    }

    CURLU* parsed = curl_url();
    if (parsed == nullptr) {
        error = "could not allocate URL validator";
        return false;
    }

    const CURLUcode set_result =
        curl_url_set(parsed, CURLUPART_URL, value.c_str(), 0);

    char* scheme = nullptr;
    char* host = nullptr;
    char* user = nullptr;
    char* password = nullptr;
    char* query = nullptr;
    char* fragment = nullptr;

    const bool parsed_ok =
        set_result == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK;

    const bool has_user =
        curl_url_get(parsed, CURLUPART_USER, &user, 0) == CURLUE_OK;
    const bool has_password =
        curl_url_get(parsed, CURLUPART_PASSWORD, &password, 0) == CURLUE_OK;
    const bool has_query =
        curl_url_get(parsed, CURLUPART_QUERY, &query, 0) == CURLUE_OK;
    const bool has_fragment =
        curl_url_get(parsed, CURLUPART_FRAGMENT, &fragment, 0) == CURLUE_OK;

    const bool valid =
        parsed_ok &&
        scheme != nullptr &&
        host != nullptr &&
        (std::string(scheme) == "http" ||
         std::string(scheme) == "https") &&
        *host != '\0' &&
        !has_user && !has_password && !has_query && !has_fragment;

    curl_free(scheme);
    curl_free(host);
    curl_free(user);
    curl_free(password);
    curl_free(query);
    curl_free(fragment);
    curl_url_cleanup(parsed);

    if (!valid) {
        error = "base_url must be a valid HTTP(S) URL without credentials, query or fragment";
    }
    return valid;
}

bool json_add_member(json_object* parent, const char* key,
                     json_object* value)
{
    if (parent == nullptr || value == nullptr) {
        if (value != nullptr) json_object_put(value);
        return false;
    }

    if (json_object_object_add(parent, key, value) != 0) {
        json_object_put(value);
        return false;
    }
    return true;
}

bool json_array_append(json_object* array, json_object* value)
{
    if (array == nullptr || value == nullptr) {
        if (value != nullptr) json_object_put(value);
        return false;
    }

    if (json_object_array_add(array, value) != 0) {
        json_object_put(value);
        return false;
    }
    return true;
}

struct ResponseBuffer {
    std::string data;
    std::size_t limit = 0;
    bool overflow = false;
    bool allocation_failed = false;
};

size_t receive_body(char* data, size_t size, size_t count, void* opaque)
{
    auto* response = static_cast<ResponseBuffer*>(opaque);
    if (response == nullptr || (size != 0 &&
        count > std::numeric_limits<std::size_t>::max() / size)) {
        return 0;
    }

    const std::size_t bytes = size * count;
    if (bytes > response->limit ||
        response->data.size() > response->limit - bytes) {
        response->overflow = true;
        return 0;
    }

    try {
        response->data.append(data, bytes);
    } catch (const std::bad_alloc&) {
        response->allocation_failed = true;
        return 0;
    } catch (...) {
        response->allocation_failed = true;
        return 0;
    }

    return bytes;
}

bool valid_config(const LlamaCppConfig& config, std::string& error)
{
    if (!valid_base_url(config.base_url, error)) {
        return false;
    }

    if (config.model.empty() || config.model.size() > 512 ||
        config.model.find('\0') != std::string::npos) {
        error = "model must contain between 1 and 512 characters and no NUL";
        return false;
    }

    if (config.connect_timeout_ms == 0 ||
        config.request_timeout_ms == 0 ||
        config.connect_timeout_ms > config.request_timeout_ms) {
        error = "invalid connection or request timeout";
        return false;
    }

    if (config.max_response_bytes == 0 ||
        config.max_response_bytes > 64U * 1024U * 1024U) {
        error = "max_response_bytes must be between 1 byte and 64 MiB";
        return false;
    }

    if (config.max_request_bytes == 0 ||
        config.max_request_bytes > 64U * 1024U * 1024U) {
        error = "max_request_bytes must be between 1 byte and 64 MiB";
        return false;
    }

    if (config.api_key.find('\r') != std::string::npos ||
        config.api_key.find('\n') != std::string::npos ||
        config.api_key.find('\0') != std::string::npos) {
        error = "api_key must not contain CR or LF or NUL characters";
        return false;
    }

    if (!std::isfinite(config.temperature) ||
        config.temperature < 0.0 || config.temperature > 2.0) {
        error = "temperature must be between 0.0 and 2.0";
        return false;
    }

    if (config.max_tokens == 0 || config.max_tokens > 131072) {
        error = "max_tokens must be between 1 and 131072";
        return false;
    }

    return true;
}

std::string make_url(const std::string& base, const char* path)
{
    std::string result = base;
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    result += path;
    return result;
}

bool json_string_member(json_object* object, const char* key,
                         std::string& value)
{
    json_object* member = nullptr;
    if (!json_object_object_get_ex(object, key, &member) ||
        member == nullptr ||
        json_object_get_type(member) != json_type_string) {
        return false;
    }

    const char* text = json_object_get_string(member);
    if (text == nullptr) {
        return false;
    }

    value.assign(text, static_cast<std::size_t>(
        json_object_get_string_len(member)));
    return true;
}

unsigned int json_unsigned_member(json_object* object, const char* key)
{
    json_object* member = nullptr;
    if (!json_object_object_get_ex(object, key, &member) ||
        member == nullptr ||
        json_object_get_type(member) != json_type_int) {
        return 0;
    }

    const auto value = json_object_get_int64(member);
    if (value <= 0 ||
        static_cast<std::uint64_t>(value) >
            std::numeric_limits<unsigned int>::max()) {
        return 0;
    }

    return static_cast<unsigned int>(value);
}

} // namespace

class LlamaCppClient::Impl {
public:
    explicit Impl(LlamaCppConfig value)
        : config(std::move(value))
    {
        while (!config.base_url.empty() && config.base_url.back() == '/') {
            config.base_url.pop_back();
        }
    }

    LlamaCppConfig config;

    bool request(const std::string& url,
                 const std::string* body,
                 std::string& response,
                 long& status,
                 std::string& error)
    {
        initialize_curl();
        if (curl_init_result != CURLE_OK) {
            error = "libcurl global initialization failed";
            return false;
        }

        CURL* handle = curl_easy_init();
        if (handle == nullptr) {
            error = "could not create libcurl handle";
            return false;
        }

        ResponseBuffer buffer;
        buffer.limit = config.max_response_bytes;

        struct curl_slist* headers = nullptr;
        if (body != nullptr) {
            headers = curl_slist_append(headers, "Content-Type: application/json");
            if (headers == nullptr) {
                curl_easy_cleanup(handle);
                error = "could not allocate HTTP headers";
                return false;
            }
        }

        if (!config.api_key.empty()) {
            const std::string authorization = "Authorization: Bearer " +
                                               config.api_key;
            struct curl_slist* updated =
                curl_slist_append(headers, authorization.c_str());
            if (updated == nullptr) {
                curl_slist_free_all(headers);
                curl_easy_cleanup(handle);
                error = "could not allocate authorization header";
                return false;
            }
            headers = updated;
        }

#define MCP_CURL_SETOPT(option, value) do { \
    const CURLcode option_code = curl_easy_setopt(handle, option, value); \
    if (option_code != CURLE_OK) { \
        if (headers != nullptr) curl_slist_free_all(headers); \
        curl_easy_cleanup(handle); \
        error = std::string("libcurl option failed: ") + \
                curl_easy_strerror(option_code); \
        return false; \
    } \
} while (false)

        MCP_CURL_SETOPT(CURLOPT_URL, url.c_str());
        MCP_CURL_SETOPT(CURLOPT_NOSIGNAL, 1L);
        MCP_CURL_SETOPT(CURLOPT_CONNECTTIMEOUT_MS,
                        static_cast<long>(config.connect_timeout_ms));
        MCP_CURL_SETOPT(CURLOPT_TIMEOUT_MS,
                        static_cast<long>(config.request_timeout_ms));
        MCP_CURL_SETOPT(CURLOPT_FOLLOWLOCATION, 0L);
        MCP_CURL_SETOPT(CURLOPT_PROTOCOLS_STR, "http,https");
        MCP_CURL_SETOPT(CURLOPT_WRITEFUNCTION, receive_body);
        MCP_CURL_SETOPT(CURLOPT_WRITEDATA, &buffer);
        MCP_CURL_SETOPT(CURLOPT_USERAGENT, "MCP-Core-LlamaClient/1.0");

        if (headers != nullptr) {
            MCP_CURL_SETOPT(CURLOPT_HTTPHEADER, headers);
        }

        if (body != nullptr) {
            MCP_CURL_SETOPT(CURLOPT_POST, 1L);
            MCP_CURL_SETOPT(CURLOPT_POSTFIELDS, body->data());
            MCP_CURL_SETOPT(CURLOPT_POSTFIELDSIZE_LARGE,
                            static_cast<curl_off_t>(body->size()));
        }

#undef MCP_CURL_SETOPT

        const CURLcode result = curl_easy_perform(handle);
        CURLcode info_result = CURLE_OK;
        if (result == CURLE_OK) {
            info_result =
                curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        }

        if (headers != nullptr) {
            curl_slist_free_all(headers);
        }
        curl_easy_cleanup(handle);

        if (buffer.overflow) {
            error = "HTTP response exceeded max_response_bytes";
            return false;
        }
        if (buffer.allocation_failed) {
            error = "could not allocate HTTP response buffer";
            return false;
        }
        if (result != CURLE_OK) {
            error = std::string("HTTP transport error: ") +
                    curl_easy_strerror(result);
            return false;
        }
        if (info_result != CURLE_OK) {
            error = std::string("HTTP status retrieval failed: ") +
                    curl_easy_strerror(info_result);
            return false;
        }

        response = std::move(buffer.data);
        if (status < 200 || status >= 300) {
            std::ostringstream stream;
            stream << "HTTP server returned status " << status;
            if (!response.empty()) {
                constexpr std::size_t max_error_body = 512;
                stream << ": " << response.substr(0, max_error_body);
            }
            error = stream.str();
            return false;
        }

        return true;
    }
};

LlamaCppClient::LlamaCppClient(const LlamaCppConfig& config)
    : impl_(new Impl(config))
{
}

LlamaCppClient::~LlamaCppClient() = default;
LlamaCppClient::LlamaCppClient(LlamaCppClient&&) noexcept = default;
LlamaCppClient& LlamaCppClient::operator=(LlamaCppClient&&) noexcept = default;

bool LlamaCppClient::health(std::string& error)
{
    error.clear();
    if (!impl_) {
        error = "client is not initialized";
        return false;
    }
    if (!valid_config(impl_->config, error)) {
        return false;
    }

    std::string response;
    long status = 0;
    return impl_->request(make_url(impl_->config.base_url, "/health"),
                          nullptr, response, status, error);
}

bool LlamaCppClient::chat_completion(
    const std::vector<LlamaMessage>& messages,
    LlamaCompletion& completion,
    std::string& error)
{
    error.clear();
    completion = LlamaCompletion{};

    if (!impl_) {
        error = "client is not initialized";
        return false;
    }
    if (!valid_config(impl_->config, error)) {
        return false;
    }
    if (messages.empty() || messages.size() > 128) {
        error = "messages must contain between 1 and 128 entries";
        return false;
    }

    std::size_t total_message_bytes = 0;
    for (const auto& message : messages) {
        if (message.role != "system" && message.role != "user" &&
            message.role != "assistant") {
            error = "unsupported message role";
            return false;
        }
        if (message.content.empty() ||
            message.content.size() > impl_->config.max_request_bytes ||
            total_message_bytes >
                impl_->config.max_request_bytes - message.content.size()) {
            error = "message content exceeds max_request_bytes";
            return false;
        }
        total_message_bytes += message.content.size();
    }

    json_object* root = json_object_new_object();
    json_object* message_array = json_object_new_array();
    if (root == nullptr || message_array == nullptr) {
        if (root != nullptr) json_object_put(root);
        if (message_array != nullptr) json_object_put(message_array);
        error = "could not allocate completion request JSON";
        return false;
    }

    const bool root_ok =
        json_add_member(root, "model",
            json_object_new_string_len(
                impl_->config.model.data(),
                static_cast<int>(impl_->config.model.size()))) &&
        json_add_member(root, "temperature",
            json_object_new_double(impl_->config.temperature)) &&
        json_add_member(root, "max_tokens",
            json_object_new_int64(impl_->config.max_tokens)) &&
        json_add_member(root, "stream",
            json_object_new_boolean(false));

    if (!root_ok) {
        json_object_put(root);
        json_object_put(message_array);
        error = "could not construct completion request JSON";
        return false;
    }

    for (const auto& message : messages) {
        json_object* item = json_object_new_object();
        if (item == nullptr) {
            json_object_put(root);
            json_object_put(message_array);
            error = "could not allocate message JSON";
            return false;
        }

        const bool item_ok =
            json_add_member(item, "role",
                json_object_new_string(message.role.c_str())) &&
            json_add_member(item, "content",
                json_object_new_string_len(
                    message.content.data(),
                    static_cast<int>(message.content.size())));

        if (!item_ok) {
            json_object_put(item);
            json_object_put(root);
            json_object_put(message_array);
            error = "could not construct message JSON";
            return false;
        }

        if (!json_array_append(message_array, item)) {
            json_object_put(root);
            json_object_put(message_array);
            error = "could not append message to request JSON";
            return false;
        }
    }

    if (!json_add_member(root, "messages", message_array)) {
        json_object_put(root);
        error = "could not attach messages to request JSON";
        return false;
    }

    const char* serialized = json_object_to_json_string_ext(
        root, JSON_C_TO_STRING_PLAIN);
    if (serialized == nullptr) {
        json_object_put(root);
        error = "could not serialize completion request";
        return false;
    }

    const std::string request_body(serialized);
    json_object_put(root);

    if (request_body.size() > impl_->config.max_request_bytes) {
        error = "serialized request exceeded max_request_bytes";
        return false;
    }

    std::string response;
    long status = 0;
    if (!impl_->request(
            make_url(impl_->config.base_url, "/v1/chat/completions"),
            &request_body, response, status, error)) {
        return false;
    }

    json_tokener* tokener = json_tokener_new();
    if (tokener == nullptr) {
        error = "could not allocate JSON parser";
        return false;
    }

    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
    json_object* parsed = json_tokener_parse_ex(
        tokener, response.data(), static_cast<int>(response.size()));
    const auto parse_error = json_tokener_get_error(tokener);
    const std::size_t parse_end = json_tokener_get_parse_end(tokener);
    json_tokener_free(tokener);

    bool trailing_data = false;
    for (std::size_t i = parse_end; i < response.size(); ++i) {
        if (!std::isspace(static_cast<unsigned char>(response[i]))) {
            trailing_data = true;
            break;
        }
    }

    if (parse_error != json_tokener_success || parsed == nullptr ||
        trailing_data ||
        json_object_get_type(parsed) != json_type_object) {
        if (parsed != nullptr) json_object_put(parsed);
        error = "server returned invalid completion JSON";
        return false;
    }

    json_object* choices = nullptr;
    json_object* first_choice = nullptr;
    json_object* message = nullptr;
    std::string content;
    const bool valid =
        json_object_object_get_ex(parsed, "choices", &choices) &&
        choices != nullptr &&
        json_object_get_type(choices) == json_type_array &&
        json_object_array_length(choices) > 0 &&
        (first_choice = json_object_array_get_idx(choices, 0)) != nullptr &&
        json_object_get_type(first_choice) == json_type_object &&
        json_object_object_get_ex(first_choice, "message", &message) &&
        message != nullptr &&
        json_object_get_type(message) == json_type_object &&
        json_string_member(message, "content", content);

    if (!valid) {
        json_object_put(parsed);
        error = "completion JSON is missing choices[0].message.content";
        return false;
    }

    completion.content = std::move(content);
    json_string_member(parsed, "model", completion.model);

    json_object* usage = nullptr;
    if (json_object_object_get_ex(parsed, "usage", &usage) &&
        usage != nullptr && json_object_get_type(usage) == json_type_object) {
        completion.prompt_tokens =
            json_unsigned_member(usage, "prompt_tokens");
        completion.completion_tokens =
            json_unsigned_member(usage, "completion_tokens");
    }

    json_object_put(parsed);
    return true;
}

} // namespace mcp
