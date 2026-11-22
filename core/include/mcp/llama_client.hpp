#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace mcp {

struct LlamaCppConfig {
    std::string base_url = "http://127.0.0.1:8080";
    std::string model = "qwen-local";
    std::string api_key;

    unsigned int connect_timeout_ms = 2000;
    unsigned int request_timeout_ms = 120000;
    std::size_t max_response_bytes = 4U * 1024U * 1024U;
    std::size_t max_request_bytes = 2U * 1024U * 1024U;

    double temperature = 0.2;
    unsigned int max_tokens = 1024;
};

struct LlamaMessage {
    std::string role;
    std::string content;
};

struct LlamaCompletion {
    std::string content;
    std::string model;
    unsigned int prompt_tokens = 0;
    unsigned int completion_tokens = 0;
};

class LlamaCppClient {
public:
    explicit LlamaCppClient(
        const LlamaCppConfig& config = LlamaCppConfig{});
    ~LlamaCppClient();

    LlamaCppClient(const LlamaCppClient&) = delete;
    LlamaCppClient& operator=(const LlamaCppClient&) = delete;
    LlamaCppClient(LlamaCppClient&&) noexcept;
    LlamaCppClient& operator=(LlamaCppClient&&) noexcept;

    bool health(std::string& error);

    bool chat_completion(
        const std::vector<LlamaMessage>& messages,
        LlamaCompletion& completion,
        std::string& error);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcp
