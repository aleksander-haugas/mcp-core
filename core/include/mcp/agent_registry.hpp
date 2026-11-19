#ifndef MCP_AGENT_REGISTRY_HPP
#define MCP_AGENT_REGISTRY_HPP

#include "mcp/agent_protocol.hpp"

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace mcp {

struct AgentRegistration {
    AgentIdentity identity;
    std::string principal_id;
    std::string transport_id;
    bool active = false;
};

class AgentRegistry {
public:
    AgentRegistry() = default;

    AgentRegistry(const AgentRegistry&) = delete;
    AgentRegistry& operator=(const AgentRegistry&) = delete;

    bool register_agent(
        const AgentRegistration& registration,
        std::string& error
    );

    bool unregister_agent(
        const AgentIdentity& identity,
        std::string& error
    );

    bool resolve(
        const AgentIdentity& identity,
        AgentRegistration& registration
    ) const;

    bool resolve_principal(
        const std::string& principal_id,
        AgentRegistration& registration
    ) const;

    bool set_active(
        const AgentIdentity& identity,
        bool active,
        std::string& error
    );

    std::size_t size() const;

private:
    using IdentityKey = std::pair<std::string, std::string>;

    static bool valid_identifier(
        const std::string& value
    );

    static IdentityKey make_key(
        const AgentIdentity& identity
    );

    mutable std::mutex mutex_;

    std::map<IdentityKey, AgentRegistration> agents_;
    std::map<std::string, IdentityKey> principals_;
};

} // namespace mcp

#endif
