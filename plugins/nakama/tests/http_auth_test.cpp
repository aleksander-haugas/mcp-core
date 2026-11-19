#include "nakama/http_auth.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

bool send_all(int fd, const std::string& data)
{
    std::size_t offset = 0;

    while (offset < data.size()) {
        const ssize_t sent = send(
            fd,
            data.data() + offset,
            data.size() - offset,
            0
        );

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (sent == 0) {
            return false;
        }

        offset += static_cast<std::size_t>(sent);
    }

    return true;
}

std::string lowercase(std::string value)
{
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    return value;
}

bool receive_request(int fd, std::string& request)
{
    request.clear();

    char buffer[4096];
    std::size_t header_end = std::string::npos;
    std::size_t content_length = 0;

    while (header_end == std::string::npos) {
        const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);

        if (received <= 0) {
            return false;
        }

        request.append(buffer, static_cast<std::size_t>(received));

        if (request.size() > 65536) {
            return false;
        }

        header_end = request.find("\r\n\r\n");
    }

    const std::string headers = lowercase(
        request.substr(0, header_end)
    );

    const std::string marker = "content-length:";

    const std::size_t position = headers.find(marker);

    if (position != std::string::npos) {
        const std::size_t start = position + marker.size();
        const std::size_t end = headers.find("\r\n", start);

        try {
            content_length = static_cast<std::size_t>(
                std::stoull(headers.substr(start, end - start))
            );
        } catch (...) {
            return false;
        }
    }

    const std::size_t body_start = header_end + 4;

    while (request.size() - body_start < content_length) {
        const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);

        if (received <= 0) {
            return false;
        }

        request.append(buffer, static_cast<std::size_t>(received));

        if (request.size() > 65536 + content_length) {
            return false;
        }
    }

    return true;
}

bool has_header(
    const std::string& request,
    const std::string& expected
)
{
    return lowercase(request).find(lowercase(expected)) !=
        std::string::npos;
}

class MockHttpServer {
public:
    explicit MockHttpServer(
        int final_status,
        std::string response_body = {}
    )
    : final_status_(final_status),
    response_body_(std::move(response_body))
    {
    }

    ~MockHttpServer()
    {
        stop();
    }

    bool start()
    {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0) {
            return false;
        }

        int reuse = 1;

        if (setsockopt(
            fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)
        ) < 0) {
            close(fd);
            return false;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;

        if (bind(
            fd,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ) < 0 || listen(fd, 1) < 0) {
            close(fd);
            return false;
        }

        socklen_t length = sizeof(address);

        if (getsockname(
            fd,
            reinterpret_cast<sockaddr*>(&address),
            &length
        ) < 0) {
            close(fd);
            return false;
        }

        listen_fd_ = fd;
        port_ = ntohs(address.sin_port);

        try {
            worker_ = std::thread(&MockHttpServer::run, this);
        } catch (...) {
            close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        return true;
    }

    void stop()
    {
        if (listen_fd_ >= 0) {
            shutdown(listen_fd_, SHUT_RDWR);
            close(listen_fd_);
            listen_fd_ = -1;
        }

        if (client_fd_.load() >= 0) {
            shutdown(client_fd_.load(), SHUT_RDWR);
        }

        if (worker_.joinable()) {
            worker_.join();
        }
    }

    unsigned short port() const
    {
        return port_;
    }

    bool valid_request() const
    {
        return valid_request_.load();
    }

    void set_large_response()
    {
        response_body_.assign(1024 * 1024 + 1, 'A');
    }

private:
    void run()
    {
        const int client = accept(listen_fd_, nullptr, nullptr);

        if (client < 0) {
            return;
        }

        client_fd_.store(client);

        std::string request;

        if (!receive_request(client, request)) {
            close(client);
            client_fd_.store(-1);
            return;
        }

        if (!has_header(request, "Authorization: Basic ")) {
            const std::string challenge =
                "HTTP/1.1 401 Unauthorized\r\n"
                "WWW-Authenticate: Basic realm=\"Nakama\"\r\n"
                "Content-Length: 0\r\n"
                "Connection: keep-alive\r\n\r\n";

            if (!send_all(client, challenge)) {
                close(client);
                client_fd_.store(-1);
                return;
            }

            if (!receive_request(client, request)) {
                close(client);
                client_fd_.store(-1);
                return;
            }
        }

        const bool request_valid =
            request.find(
                "POST /v2/account/authenticate/device?create=false "
            ) == 0 &&
            has_header(request, "Authorization: Basic ZGVmYXVsdGtleTo=") &&
            has_header(request, "Content-Type: application/json") &&
            has_header(request, "Accept: application/json") &&
            request.find(
                R"("id":"c85aee1c-e1d6-11f1-8e99-a8b13b6f67b2")"
            ) != std::string::npos;

        valid_request_.store(request_valid);

        const std::string body = response_body_.empty()
        ? (
            final_status_ == 200
            ? R"({"token":"mock.jwt.token"})"
            : R"({"error":"unauthorized"})"
        )
        : response_body_;

        const std::string reason =
            final_status_ == 200 ? "OK" : "Unauthorized";

        const std::string response =
            "HTTP/1.1 " + std::to_string(final_status_) +
            " " + reason + "\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;

        (void)send_all(client, response);

        shutdown(client, SHUT_RDWR);
        close(client);
        client_fd_.store(-1);
    }

    int listen_fd_ = -1;
    std::atomic<int> client_fd_{-1};
    unsigned short port_ = 0;
    int final_status_;
    std::string response_body_;
    std::atomic<bool> valid_request_{false};
    std::thread worker_;
};

bool check(bool condition, const char* description)
{
    std::cout
        << (condition ? "[PASS] " : "[FAIL] ")
        << description << '\n';

    return condition;
}

nakama::HttpAuthConfig make_config(unsigned short port)
{
    nakama::HttpAuthConfig config;
    config.server_url =
        "ws://127.0.0.1:" + std::to_string(port);
    config.server_key = "defaultkey";
    config.device_id =
        "c85aee1c-e1d6-11f1-8e99-a8b13b6f67b2";
    config.connect_timeout_ms = 3000;
    config.verify_tls = false;

    return config;
}

} // namespace

int main()
{
    int failures = 0;

    {
        MockHttpServer server(200);

        if (!check(server.start(), "Start local HTTP authentication server")) {
            return 1;
        }

        nakama::HttpAuthConfig config = make_config(server.port());

        std::string token;
        std::string error;

        const bool authenticated =
            nakama::authenticate_device(config, token, error);

        if (!check(authenticated, "Authenticate device through Basic Auth")) {
            ++failures;
            std::cerr << "Authentication error: " << error << '\n';
        }

        if (!check(
            token == "mock.jwt.token",
            "Extract JWT from HTTP response"
        )) {
            ++failures;
        }

        if (!check(
            server.valid_request(),
            "Validate Basic Auth, JSON headers, endpoint and device ID"
        )) {
            ++failures;
        }

        server.stop();
    }

    {
        MockHttpServer server(401);

        if (!check(server.start(), "Start HTTP rejection server")) {
            return 1;
        }

        nakama::HttpAuthConfig config = make_config(server.port());

        std::string token;
        std::string error;

        const bool authenticated =
            nakama::authenticate_device(config, token, error);

        if (!check(
            !authenticated && token.empty(),
            "Reject HTTP 401 without returning a token"
        )) {
            ++failures;
        }

        server.stop();
    }

    {
        MockHttpServer server(200, R"({"token":)");

            if (!check(server.start(), "Start malformed JSON server")) {
                return 1;
            }

            auto config = make_config(server.port());

            std::string token;
            std::string error;

            const bool authenticated =
            nakama::authenticate_device(config, token, error);

            if (!check(
                !authenticated && token.empty(),
                       "Reject malformed JSON authentication response"
            )) {
                ++failures;
            }

            server.stop();
        }

        {
            MockHttpServer server(200, R"({"user_id":"123"})");

            if (!check(server.start(), "Start missing-token server")) {
                return 1;
            }

            auto config = make_config(server.port());

            std::string token;
            std::string error;

            const bool authenticated =
            nakama::authenticate_device(config, token, error);

            if (!check(
                !authenticated && token.empty(),
                       "Reject authentication response without token"
            )) {
                ++failures;
            }

            server.stop();
        }

        {
            MockHttpServer server(200, R"({"token":""})");

            if (!check(server.start(), "Start empty-token server")) {
                return 1;
            }

            auto config = make_config(server.port());

            std::string token;
            std::string error;

            const bool authenticated =
            nakama::authenticate_device(config, token, error);

            if (!check(
                !authenticated && token.empty(),
                       "Reject empty authentication token"
            )) {
                ++failures;
            }

            server.stop();
        }

        {
            MockHttpServer server(200);

            server.set_large_response();

            if (!check(server.start(), "Start oversized response server")) {
                return 1;
            }

            auto config = make_config(server.port());

            std::string token;
            std::string error;

            const bool authenticated =
            nakama::authenticate_device(config, token, error);

            if (!check(
                !authenticated && token.empty(),
                       "Reject HTTP response exceeding 1 MiB"
            )) {
                ++failures;
            }

            if (!check(
                error == "Authentication response exceeded size limit",
                "Report HTTP response size limit violation"
            )) {
                ++failures;
            }

            server.stop();
        }

        {
            auto config = make_config(7350);
            config.verify_tls = true;

            std::string token;
            std::string error;

            const bool authenticated =
            nakama::authenticate_device(config, token, error);

            if (!check(
                !authenticated && token.empty() &&
                error == "Invalid Nakama URL or TLS configuration",
                "Reject ws:// with TLS verification enabled"
            )) {
                ++failures;
            }
        }

        {
            auto config = make_config(7350);
            config.server_url = "wss://127.0.0.1:7350";
            config.verify_tls = false;

            std::string token;
            std::string error;

            const bool authenticated =
            nakama::authenticate_device(config, token, error);

            if (!check(
                !authenticated && token.empty() &&
                error == "Invalid Nakama URL or TLS configuration",
                "Reject wss:// with TLS verification disabled"
            )) {
                ++failures;
            }
        }

        {
            auto config = make_config(7350);
            config.server_key.assign(257, 'A');

            std::string token;
            std::string error;

            const bool authenticated =
            nakama::authenticate_device(config, token, error);

            if (!check(
                !authenticated && token.empty() &&
                error == "Invalid authentication configuration",
                "Reject oversized server key"
            )) {
                ++failures;
            }
        }

    std::cout
        << "\nHTTP authentication tests: "
        << (failures == 0 ? "PASS" : "FAIL")
        << " (" << failures << " failures)\n";

    return failures == 0 ? 0 : 1;
}
