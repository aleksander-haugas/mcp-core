#ifndef MCP_AGENT_TRANSPORT_HPP
#define MCP_AGENT_TRANSPORT_HPP

#include "mcp/agent_registry.hpp"

#include <functional>
#include <string>

namespace mcp {

class AgentTransport {
public:
    using MessageHandler = std::function<void(
        const std::string& authenticated_principal,
        const std::string& message_json
    )>;

    AgentTransport() = default;
    virtual ~AgentTransport() = default;

    AgentTransport(const AgentTransport&) = delete;
    AgentTransport& operator=(const AgentTransport&) = delete;

    virtual std::string transport_id() const = 0;

    virtual bool start(
        MessageHandler handler,
        std::string& error
    ) = 0;

    // send() must have a bounded execution time.
    virtual bool send(
        const AgentRegistration& destination,
        const std::string& message_json,
        std::string& error
    ) = 0;

    // Stop the worker and prevent further message delivery.
    // Must not throw.
    virtual void stop() noexcept = 0;
};

} // namespace mcp

#endif
