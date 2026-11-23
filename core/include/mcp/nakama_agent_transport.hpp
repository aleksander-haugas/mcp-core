#ifndef MCP_NAKAMA_AGENT_TRANSPORT_HPP
#define MCP_NAKAMA_AGENT_TRANSPORT_HPP

#include "mcp/agent_transport.hpp"
#include "mcp/plugin_manager.hpp"

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace mcp {

class NakamaAgentTransport final : public AgentTransport {
public:
    explicit NakamaAgentTransport(PluginManager& plugin_manager);
    ~NakamaAgentTransport() override;

    NakamaAgentTransport(const NakamaAgentTransport&) = delete;
    NakamaAgentTransport& operator=(const NakamaAgentTransport&) = delete;

    std::string transport_id() const override;

    bool start(
        MessageHandler handler,
        std::string& error
    ) override;

    bool send(
        const AgentRegistration& destination,
        const std::string& message_json,
        std::string& error
    ) override;

    void stop() noexcept override;

private:
    void receive(
        const AuthenticatedPluginMessage& message
    ) noexcept;

    void finish_send() noexcept;

    PluginManager& plugin_manager_;

    std::mutex lifecycle_mutex_;
    std::mutex mutex_;
    std::condition_variable callbacks_drained_;

    MessageHandler handler_;

    std::size_t active_callbacks_ = 0;
    std::size_t active_sends_ = 0;
    bool started_ = false;
    bool accepting_ = false;
};

} // namespace mcp

#endif
