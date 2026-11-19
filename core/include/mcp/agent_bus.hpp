#ifndef MCP_AGENT_BUS_HPP
#define MCP_AGENT_BUS_HPP

#include "mcp/agent_protocol.hpp"
#include "mcp/agent_registry.hpp"
#include "mcp/agent_transport.hpp"

#include <condition_variable>
#include <functional>
#include <memory>
#include <string>

namespace mcp {

class AgentBus {
public:
    using MessageHandler = std::function<void(
        const AgentRegistration& source,
        const AgentMessage& message
    )>;

    explicit AgentBus(AgentRegistry& registry);

    ~AgentBus();

    AgentBus(const AgentBus&) = delete;
    AgentBus& operator=(const AgentBus&) = delete;

    // Lifecycle operations are serialized. A MessageHandler callback
    // must not synchronously call add_transport(), remove_transport(),
    // or stop() on this same AgentBus.
    bool add_transport(
        std::shared_ptr<AgentTransport> transport,
        std::string& error
    );

    bool remove_transport(
        const std::string& transport_id,
        std::string& error
    );

    void set_message_handler(MessageHandler handler);

    bool send(
        const std::string& authenticated_principal,
        const std::string& message_json,
        std::string& error
    );

    void stop() noexcept;

private:
    struct SharedState;

    static void finish_send(
        const std::shared_ptr<SharedState>& state,
        const std::string& transport_id
    ) noexcept;

    static void receive(
        const std::weak_ptr<SharedState>& weak_state,
        const std::string& transport_id,
        const std::string& authenticated_principal,
        const std::string& message_json
    ) noexcept;

    std::shared_ptr<SharedState> state_;
};

} // namespace mcp

#endif
