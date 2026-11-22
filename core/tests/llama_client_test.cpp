#include "mcp/llama_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class MockServer {
public:
    MockServer(std::string body, int status = 200,
               unsigned int delay_ms = 0, unsigned int requests = 1)
        : body_(std::move(body)), status_(status),
          delay_ms_(delay_ms), requests_(requests)
    {
        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket_ < 0) throw std::runtime_error("socket failed");

        int reuse = 1;
        setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;

        if (::bind(socket_, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) != 0 ||
            ::listen(socket_, 16) != 0) {
            ::close(socket_);
            throw std::runtime_error("bind/listen failed");
        }

        socklen_t length = sizeof(address);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address),
                        &length) != 0) {
            ::close(socket_);
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { serve(); });
    }

    ~MockServer()
    {
        if (socket_ >= 0) {
            ::shutdown(socket_, SHUT_RDWR);
            ::close(socket_);
            socket_ = -1;
        }
        if (worker_.joinable()) worker_.join();
    }

    std::string url() const
    {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

private:
    void serve()
    {
        for (unsigned int i = 0; i < requests_; ++i) {
            const int client = ::accept(socket_, nullptr, nullptr);
            if (client < 0) return;

            int no_sigpipe = 1;
            (void)setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE,
                             &no_sigpipe, sizeof(no_sigpipe));
            char buffer[4096];
            (void)::recv(client, buffer, sizeof(buffer), 0);

            if (delay_ms_ != 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(delay_ms_));
            }

            const std::string reason = status_ >= 200 && status_ < 300
                ? "OK" : "Error";
            const std::string response =
                "HTTP/1.1 " + std::to_string(status_) + " " + reason +
                "\r\nContent-Type: application/json\r\nConnection: close\r\n"
                "Content-Length: " + std::to_string(body_.size()) +
                "\r\n\r\n" + body_;

            std::size_t sent = 0;
            while (sent < response.size()) {
                const auto count = ::send(client, response.data() + sent,
                                          response.size() - sent, 0);
                if (count <= 0) break;
                sent += static_cast<std::size_t>(count);
            }
            ::close(client);
        }
    }

    int socket_ = -1;
    unsigned short port_ = 0;
    std::string body_;
    int status_;
    unsigned int delay_ms_;
    unsigned int requests_;
    std::thread worker_;
};

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

mcp::LlamaCppConfig config_for(const std::string& url)
{
    mcp::LlamaCppConfig config;
    config.base_url = url;
    config.connect_timeout_ms = 500;
    config.request_timeout_ms = 1500;
    return config;
}

void test_health()
{
    MockServer server(R"({"status":"ok"})");
    mcp::LlamaCppClient client(config_for(server.url()));
    std::string error;
    require(client.health(error), error.c_str());
}

void test_completion()
{
    MockServer server(
        R"({"model":"qwen-local","choices":[{"message":{"content":"pong"}}],)"
        R"("usage":{"prompt_tokens":12,"completion_tokens":3}})");
    mcp::LlamaCppClient client(config_for(server.url()));
    mcp::LlamaCompletion completion;
    std::string error;

    require(client.chat_completion({{"user", "ping"}}, completion, error),
            error.c_str());
    require(completion.content == "pong", "unexpected completion content");
    require(completion.model == "qwen-local", "unexpected model");
    require(completion.prompt_tokens == 12, "unexpected prompt token count");
    require(completion.completion_tokens == 3,
            "unexpected completion token count");
}

void test_http_error()
{
    MockServer server(R"({"error":"unavailable"})", 503);
    mcp::LlamaCppClient client(config_for(server.url()));
    std::string error;
    require(!client.health(error), "HTTP 503 must fail");
    require(error.find("503") != std::string::npos,
            "HTTP error should report status");
}

void test_invalid_json()
{
    MockServer server("not-json");
    mcp::LlamaCppClient client(config_for(server.url()));
    mcp::LlamaCompletion completion;
    std::string error;
    require(!client.chat_completion({{"user", "ping"}}, completion, error),
            "invalid JSON must fail");
}

void test_invalid_schema()
{
    MockServer server(R"({"choices":[]})");
    mcp::LlamaCppClient client(config_for(server.url()));
    mcp::LlamaCompletion completion;
    std::string error;
    require(!client.chat_completion({{"user", "ping"}}, completion, error),
            "invalid completion schema must fail");
}

void test_timeout()
{
    MockServer server(R"({"status":"ok"})", 200, 400);
    auto config = config_for(server.url());
    config.request_timeout_ms = 100;
    config.connect_timeout_ms = 50;
    mcp::LlamaCppClient client(config);
    std::string error;
    require(!client.health(error), "request timeout must fail");
}

void test_response_limit()
{
    MockServer server(R"({"status":"this-response-is-too-large"})");
    auto config = config_for(server.url());
    config.max_response_bytes = 8;
    mcp::LlamaCppClient client(config);
    std::string error;
    require(!client.health(error), "oversized response must fail");
    require(error.find("max_response_bytes") != std::string::npos,
            "response limit error should be explicit");
}

void test_invalid_role()
{
    mcp::LlamaCppClient client;
    mcp::LlamaCompletion completion;
    std::string error;
    require(!client.chat_completion({{"tool", "not allowed"}},
                                    completion, error),
            "unsupported role must fail");
}

void test_api_key_rejects_cr()
{
    mcp::LlamaCppConfig config;
    config.api_key = "secret\rInjected: value";
    mcp::LlamaCppClient client(config);
    std::string error;
    require(!client.health(error), "CR in API key must be rejected");
    require(error.find("CR or LF") != std::string::npos,
            "API key rejection should identify CR/LF");
}

void test_api_key_rejects_lf()
{
    mcp::LlamaCppConfig config;
    config.api_key = "secret\nInjected: value";
    mcp::LlamaCppClient client(config);
    std::string error;
    require(!client.health(error), "LF in API key must be rejected");
    require(error.find("CR or LF") != std::string::npos,
            "API key rejection should identify CR/LF");
}

void test_request_size_limit()
{
    mcp::LlamaCppConfig config;
    config.max_request_bytes = 8;
    mcp::LlamaCppClient client(config);
    mcp::LlamaCompletion completion;
    std::string error;

    require(!client.chat_completion({{"user", "this is too large"}},
                                    completion, error),
            "oversized request must fail");
    require(error.find("max_request_bytes") != std::string::npos,
            "request size error should be explicit");
}

void test_concurrent_requests()
{
    constexpr unsigned int count = 4;
    MockServer server(
        R"({"choices":[{"message":{"content":"ok"}}]})",
        200, 0, count);

    auto config = config_for(server.url());
    mcp::LlamaCppClient client(config);
    std::vector<std::thread> workers;
    std::atomic<unsigned int> successes{0};

    for (unsigned int i = 0; i < count; ++i) {
        workers.emplace_back([&client, &successes] {
            mcp::LlamaCompletion completion;
            std::string error;
            if (client.chat_completion({{"user", "test"}},
                                       completion, error) &&
                completion.content == "ok") {
                ++successes;
            }
        });
    }

    for (auto& worker : workers) worker.join();
    require(successes.load() == count,
            "shared client should support concurrent requests");
}

void test_invalid_base_url()
{
    mcp::LlamaCppConfig config;
    config.base_url = "http://";
    mcp::LlamaCppClient client(config);
    std::string error;
    require(!client.health(error), "malformed base URL must fail");
    require(error.find("base_url") != std::string::npos,
            "invalid URL should be identified");
}

void test_api_key_rejects_nul()
{
    mcp::LlamaCppConfig config;
    config.api_key = std::string("secret\0Injected", 15);
    mcp::LlamaCppClient client(config);
    std::string error;
    require(!client.health(error), "NUL in API key must be rejected");
    require(error.find("NUL") != std::string::npos,
            "API key rejection should identify NUL");
}

void test_trailing_json_data()
{
    MockServer server(
        R"({"choices":[{"message":{"content":"ok"}}]} garbage)");
    mcp::LlamaCppClient client(config_for(server.url()));
    mcp::LlamaCompletion completion;
    std::string error;
    require(!client.chat_completion({{"user", "ping"}}, completion, error),
            "trailing non-JSON data must fail");
}

void test_exact_response_limit()
{
    const std::string body = R"({"status":"ok"})";
    MockServer server(body);
    auto config = config_for(server.url());
    config.max_response_bytes = body.size();
    mcp::LlamaCppClient client(config);
    std::string error;
    require(client.health(error), error.c_str());
}

void test_configured_large_message_limit()
{
    MockServer server(
        R"({"choices":[{"message":{"content":"ok"}}]})");
    auto config = config_for(server.url());
    config.max_request_bytes = 2U * 1024U * 1024U;
    mcp::LlamaCppClient client(config);
    mcp::LlamaCompletion completion;
    std::string error;
    const std::string content(1024U * 1024U + 1U, 'x');
    require(client.chat_completion({{"user", content}}, completion, error),
            error.c_str());
    require(completion.content == "ok", "unexpected completion");
}

void test_connection_failure()
{
    auto config = config_for("http://127.0.0.1:1");
    config.connect_timeout_ms = 300;
    config.request_timeout_ms = 500;

    mcp::LlamaCppClient client(config);
    std::string error;

    require(!client.health(error),
            "connection to an unavailable server must fail");
    require(error.find("HTTP transport error") != std::string::npos,
            "connection failure should be classified as transport error");
}

} // namespace

int main()
{
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"health endpoint", test_health},
        {"chat completion", test_completion},
        {"HTTP error handling", test_http_error},
        {"invalid JSON", test_invalid_json},
        {"invalid response schema", test_invalid_schema},
        {"request timeout", test_timeout},
        {"response size limit", test_response_limit},
        {"message role validation", test_invalid_role},
        {"API key rejects CR", test_api_key_rejects_cr},
        {"API key rejects LF", test_api_key_rejects_lf},
        {"request size limit", test_request_size_limit},
        {"concurrent shared-client requests", test_concurrent_requests},
        {"invalid base URL", test_invalid_base_url},
        {"API key rejects NUL", test_api_key_rejects_nul},
        {"trailing JSON data", test_trailing_json_data},
        {"exact response size limit", test_exact_response_limit},
        {"configured large message limit", test_configured_large_message_limit},
        {"connection failure", test_connection_failure}
    };

    unsigned int passed = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            ++passed;
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception& exception) {
            std::cerr << "[FAIL] " << test.first << ": "
                      << exception.what() << '\n';
        }
    }

    std::cout << passed << "/" << tests.size() << " tests passed\n";
    return passed == tests.size() ? 0 : 1;
}
