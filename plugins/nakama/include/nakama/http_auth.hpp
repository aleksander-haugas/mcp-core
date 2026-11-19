#ifndef NAKAMA_HTTP_AUTH_HPP
#define NAKAMA_HTTP_AUTH_HPP

#include <string>

namespace nakama {

struct HttpAuthConfig {
    std::string server_url;
    std::string server_key;
    std::string device_id;
    long connect_timeout_ms = 10000;
    bool verify_tls = true;
};

// Authenticates an existing Nakama device account.
// The returned JWT remains in memory only.
bool authenticate_device(
    const HttpAuthConfig& config,
    std::string& token,
    std::string& error
);

}

#endif
