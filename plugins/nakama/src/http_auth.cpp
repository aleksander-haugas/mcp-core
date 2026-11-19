#include "nakama/http_auth.hpp"
#include "nakama/websocket_transport.hpp"

#include <curl/curl.h>
#include <json-c/json.h>
#include <json-c/json_tokener.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace nakama {
namespace {

constexpr std::size_t MAX_RESPONSE_SIZE = 1024 * 1024;
constexpr std::size_t MAX_TOKEN_SIZE = 16384;

struct ResponseBuffer {
    std::string data;
    bool overflow = false;
};

size_t receive_response(
    char* data,
    size_t size,
    size_t count,
    void* userdata
)
{
    auto* response = static_cast<ResponseBuffer*>(userdata);

    if (response == nullptr ||
        (count != 0 &&
         size > std::numeric_limits<size_t>::max() / count)) {
        return 0;
    }

    const size_t bytes = size * count;

    if (bytes > MAX_RESPONSE_SIZE -
        (response->data.size() > MAX_RESPONSE_SIZE
            ? MAX_RESPONSE_SIZE
            : response->data.size())) {
        response->overflow = true;
        return 0;
    }

    response->data.append(data, bytes);
    return bytes;
}

std::string make_auth_url(
    const std::string& server_url,
    bool verify_tls
)
{
    const bool is_ws =
    server_url.compare(0, 5, "ws://") == 0;

    const bool is_wss =
    server_url.compare(0, 6, "wss://") == 0;

    if ((!is_ws && !is_wss) ||
        (is_ws && verify_tls) ||
        (is_wss && !verify_tls) ||
        server_url.find_first_of("?#\\") != std::string::npos) {
        return {};
        }

        for (unsigned char c : server_url) {
            if (c <= 0x20 || c == 0x7f) {
                return {};
            }
        }

        CURLU* url = curl_url();

        if (url == nullptr) {
            return {};
        }

        if (curl_url_set(
            url,
            CURLUPART_URL,
            server_url.c_str(),
                         0
        ) != CURLUE_OK) {
            curl_url_cleanup(url);
            return {};
        }

        char* scheme = nullptr;
        char* host = nullptr;
        char* path = nullptr;
        char* port = nullptr;
        char* user = nullptr;
        char* password = nullptr;
        char* query = nullptr;
        char* fragment = nullptr;

        const auto cleanup = [&]() {
            curl_free(scheme);
            curl_free(host);
            curl_free(path);
            curl_free(port);
            curl_free(user);
            curl_free(password);
            curl_free(query);
            curl_free(fragment);
            curl_url_cleanup(url);
        };

        bool valid =
        curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
        curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
        scheme != nullptr &&
        host != nullptr &&
        host[0] != '\0';

        if (valid) {
            valid =
            std::string(scheme) == (is_ws ? "ws" : "wss");
        }

        // Reject embedded credentials.
        if (valid &&
            (curl_url_get(url, CURLUPART_USER, &user, 0) == CURLUE_OK ||
            curl_url_get(url, CURLUPART_PASSWORD, &password, 0) == CURLUE_OK)) {
            valid = false;
            }

            // Reject existing query strings and fragments.
            if (valid &&
                (curl_url_get(url, CURLUPART_QUERY, &query, 0) == CURLUE_OK ||
                curl_url_get(url, CURLUPART_FRAGMENT, &fragment, 0) == CURLUE_OK)) {
                valid = false;
                }

                // Only an empty path or "/" is allowed for the server base URL.
                if (valid) {
                    const CURLUcode path_result =
                    curl_url_get(url, CURLUPART_PATH, &path, 0);

                    valid =
                    path_result == CURLUE_OK &&
                    path != nullptr &&
                    std::string(path) == "/";
                }

                // Validate an explicitly supplied port.
                if (valid) {
                    const CURLUcode port_result =
                    curl_url_get(url, CURLUPART_PORT, &port, 0);

                    if (port_result == CURLUE_OK) {
                        if (port == nullptr || port[0] == '\0') {
                            valid = false;
                        } else {
                            unsigned long port_value = 0;

                            for (const char* p = port; *p != '\0'; ++p) {
                                if (*p < '0' || *p > '9') {
                                    valid = false;
                                    break;
                                }

                                port_value =
                                port_value * 10 +
                                static_cast<unsigned long>(*p - '0');

                                if (port_value > 65535) {
                                    valid = false;
                                    break;
                                }
                            }

                            if (port_value == 0) {
                                valid = false;
                            }
                        }
                    } else if (port_result != CURLUE_NO_PORT) {
                        valid = false;
                    }
                }

                if (!valid) {
                    cleanup();
                    return {};
                }

                const char* http_scheme = is_ws ? "http" : "https";

                if (curl_url_set(
                    url,
                    CURLUPART_SCHEME,
                    http_scheme,
                    0
                ) != CURLUE_OK ||
                curl_url_set(
                    url,
                    CURLUPART_PATH,
                    "/v2/account/authenticate/device",
                    0
                ) != CURLUE_OK ||
                curl_url_set(
                    url,
                    CURLUPART_QUERY,
                    "create=false",
                    0
                ) != CURLUE_OK) {
                    cleanup();
                    return {};
                }

                char* result = nullptr;

                if (curl_url_get(url, CURLUPART_URL, &result, 0) != CURLUE_OK ||
                    result == nullptr) {
                    cleanup();
                return {};
                    }

                    std::string auth_url(result);

                    curl_free(result);
                    cleanup();

                    return auth_url;
}

bool extract_token(
    const std::string& response,
    std::string& token
)
{
    if (response.empty() ||
        response.size() > static_cast<size_t>(INT32_MAX)) {
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
        response.data(),
        static_cast<int>(response.size())
    );

    const bool parsed =
        json_tokener_get_error(tokener) == json_tokener_success &&
        root != nullptr;

    const size_t parse_end = json_tokener_get_parse_end(tokener);

    json_tokener_free(tokener);

    if (!parsed) {
        if (root != nullptr) {
            json_object_put(root);
        }
        return false;
    }

    bool valid = json_object_get_type(root) == json_type_object;

    json_object* token_object = nullptr;

    if (valid) {
        valid =
            json_object_object_get_ex(root, "token", &token_object) &&
            token_object != nullptr &&
            json_object_get_type(token_object) == json_type_string;
    }

    std::string candidate;

    if (valid) {
        const char* value = json_object_get_string(token_object);
        const int length = json_object_get_string_len(token_object);

        valid =
            value != nullptr &&
            length > 0 &&
            static_cast<size_t>(length) <= MAX_TOKEN_SIZE &&
            std::strlen(value) == static_cast<size_t>(length);

        if (valid) {
            candidate.assign(value, static_cast<size_t>(length));
        }
    }

    if (valid) {
        for (size_t i = parse_end; i < response.size(); ++i) {
            const unsigned char c =
                static_cast<unsigned char>(response[i]);

            if (c != ' ' && c != '\t' &&
                c != '\r' && c != '\n') {
                valid = false;
                break;
            }
        }
    }

    json_object_put(root);

    if (!valid) {
        return false;
    }

    token = std::move(candidate);
    return true;
}

} // namespace

bool authenticate_device(
    const HttpAuthConfig& config,
    std::string& token,
    std::string& error
)
{
    token.clear();
    error.clear();

    if (config.server_url.empty() ||
        config.server_key.empty() ||
        config.server_key.size() > 256 ||
        config.device_id.empty() ||
        config.device_id.size() > 256 ||
        config.connect_timeout_ms < 1000 ||
        config.connect_timeout_ms > 60000) {
        error = "Invalid authentication configuration";
    return false;
        }

        const bool is_ws =
        config.server_url.compare(0, 5, "ws://") == 0;

        const bool is_wss =
        config.server_url.compare(0, 6, "wss://") == 0;

        if ((!is_ws && !is_wss) ||
            (is_ws && config.verify_tls) ||
            (is_wss && !config.verify_tls)) {
            error = "Invalid Nakama URL or TLS configuration";
        return false;
            }

            const std::string url =
            make_auth_url(config.server_url, config.verify_tls);

    if (url.empty()) {
        error = "Unsupported Nakama server URL scheme";
        return false;
    }

    if (!initialize_websocket_library()) {
        error = "libcurl initialization failed";
        return false;
    }

    json_object* request = json_object_new_object();

    if (request == nullptr) {
        error = "Could not allocate authentication request";
        return false;
    }

    json_object* device_id = json_object_new_string_len(
        config.device_id.data(),
        static_cast<int>(config.device_id.size())
    );

    if (device_id == nullptr) {
        json_object_put(request);
        error = "Could not allocate device identifier";
        return false;
    }

    json_object_object_add(request, "id", device_id);

    const char* serialized =
        json_object_to_json_string_ext(
            request,
            JSON_C_TO_STRING_PLAIN
        );

    if (serialized == nullptr) {
        json_object_put(request);
        error = "Could not serialize authentication request";
        return false;
    }

    const std::string request_body(serialized);
    json_object_put(request);

    CURL* curl = curl_easy_init();

    if (curl == nullptr) {
        error = "Could not create HTTP request";
        return false;
    }

    struct curl_slist* headers = nullptr;

    struct curl_slist* updated_headers = curl_slist_append(
        headers,
        "Content-Type: application/json"
    );

    if (updated_headers == nullptr) {
        curl_easy_cleanup(curl);
        error = "Could not allocate HTTP headers";
        return false;
    }

    headers = updated_headers;

    updated_headers = curl_slist_append(
        headers,
        "Accept: application/json"
    );

    if (updated_headers == nullptr) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        error = "Could not allocate HTTP headers";
        return false;
    }

    headers = updated_headers;

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    ResponseBuffer response;
    CURLcode result = CURLE_FAILED_INIT;
    long http_status = 0;

    auto set_option = [&](CURLoption option, auto value) -> bool {
        return curl_easy_setopt(curl, option, value) == CURLE_OK;
    };

    const bool options_ok =
    set_option(CURLOPT_HTTPHEADER, headers) &&
    set_option(CURLOPT_URL, url.c_str()) &&
    set_option(CURLOPT_POST, 1L) &&
    set_option(CURLOPT_POSTFIELDS, request_body.data()) &&
    set_option(
        CURLOPT_POSTFIELDSIZE_LARGE,
        static_cast<curl_off_t>(request_body.size())
    ) &&
    set_option(CURLOPT_HTTPAUTH, CURLAUTH_BASIC) &&
    set_option(CURLOPT_USERNAME, config.server_key.c_str()) &&
    set_option(CURLOPT_PASSWORD, "") &&
    set_option(CURLOPT_CONNECTTIMEOUT_MS, config.connect_timeout_ms) &&
    set_option(CURLOPT_TIMEOUT_MS, config.connect_timeout_ms) &&
    set_option(CURLOPT_NOSIGNAL, 1L) &&
    set_option(CURLOPT_FOLLOWLOCATION, 0L) &&
    set_option(CURLOPT_MAXREDIRS, 0L) &&
    set_option(CURLOPT_PROTOCOLS_STR, "http,https") &&
    set_option(
        CURLOPT_SSL_VERIFYPEER,
        config.verify_tls ? 1L : 0L
    ) &&
    set_option(
        CURLOPT_SSL_VERIFYHOST,
        config.verify_tls ? 2L : 0L
    ) &&
    set_option(CURLOPT_WRITEFUNCTION, receive_response) &&
    set_option(CURLOPT_WRITEDATA, &response) &&
    set_option(CURLOPT_USERAGENT, "MCP-Core-Nakama/0.1");

    if (!options_ok) {
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        error = "Could not configure HTTP authentication request";
        return false;
    }



    result = curl_easy_perform(curl);

    CURLcode info_result = CURLE_OK;

    if (result == CURLE_OK) {
        info_result = curl_easy_getinfo(
            curl,
            CURLINFO_RESPONSE_CODE,
            &http_status
        );
    }

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (response.overflow) {
        error = "Authentication response exceeded size limit";
        return false;
    }

    if (result != CURLE_OK) {
        error = std::string("HTTP authentication request failed: ") +
        curl_easy_strerror(result);
        return false;
    }

    if (info_result != CURLE_OK) {
        error = "Could not read HTTP authentication response status";
        return false;
    }

    if (http_status != 200) {
        error = "Nakama authentication returned HTTP " +
        std::to_string(http_status);
        return false;
    }

    if (!extract_token(response.data, token)) {
        error = "Nakama authentication response contained no valid token";
        return false;
    }

    return true;

}

} // namespace nakama
