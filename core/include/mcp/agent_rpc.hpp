#ifndef MCP_AGENT_RPC_HPP
#define MCP_AGENT_RPC_HPP

#include "mcp/agent_bus.hpp"

#include <functional>
#include <memory>
#include <string>

namespace mcp {

class AgentRpc {
public:
    using MethodHandler = std::function<bool(
        const AgentIdentity& caller,
        const std::string& payload_json,
        std::string& result_json,
        std::string& error
    )>;

    AgentRpc(
        AgentBus& bus,
        AgentIdentity local_identity,
        std::string authenticated_principal
    );

    ~AgentRpc();

    AgentRpc(const AgentRpc&) = delete;
    AgentRpc& operator=(const AgentRpc&) = delete;

    bool register_method(
        const std::string& method,
        MethodHandler handler,
        std::string& error
    );

    bool call(
        const AgentIdentity& target,
        const std::string& method,
        const std::string& payload_json,
        std::string& result_json,
        std::string& error,
        unsigned int timeout_ms = 5000
    );

    void stop() noexcept;

private:
    struct SharedState;

    static void receive(
        const std::shared_ptr<SharedState>& state,
        const AgentRegistration& source,
        const AgentMessage& message
    ) noexcept;

    static void handle_request(
        const std::shared_ptr<SharedState>& state,
        const AgentRegistration& source,
        const AgentMessage& message
    ) noexcept;

    std::shared_ptr<SharedState> state_;
};

} // namespace mcp

#endif
