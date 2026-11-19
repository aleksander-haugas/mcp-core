#include "mcp/agent_registry.hpp"

namespace mcp {

bool AgentRegistry::valid_identifier(
    const std::string& value
)
{
    if (value.empty() ||
        value.size() > agent_protocol::MAX_IDENTIFIER_SIZE) {
        return false;
    }

    for (unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7f) {
            return false;
        }
    }

    return true;
}

AgentRegistry::IdentityKey AgentRegistry::make_key(
    const AgentIdentity& identity
)
{
    return {
        identity.agent_id,
        identity.instance_id
    };
}

bool AgentRegistry::register_agent(
    const AgentRegistration& registration,
    std::string& error
)
{
    error.clear();

    if (!valid_identifier(registration.identity.agent_id)) {
        error = "Invalid agent_id";
        return false;
    }

    if (!valid_identifier(registration.identity.instance_id)) {
        error = "Invalid instance_id";
        return false;
    }

    if (!valid_identifier(registration.principal_id)) {
        error = "Invalid principal_id";
        return false;
    }

    if (!valid_identifier(registration.transport_id)) {
        error = "Invalid transport_id";
        return false;
    }

    const IdentityKey key = make_key(registration.identity);

    std::lock_guard<std::mutex> lock(mutex_);

    if (agents_.find(key) != agents_.end()) {
        error = "Agent instance is already registered";
        return false;
    }

    if (principals_.find(registration.principal_id) !=
        principals_.end()) {
        error = "Principal is already associated with an agent";
        return false;
    }

    const auto agent_result = agents_.emplace(
        key,
        registration
    );

    if (!agent_result.second) {
        error = "Agent instance registration failed";
        return false;
    }

    try {
        const auto principal_result = principals_.emplace(
            registration.principal_id,
            key
        );

        if (!principal_result.second) {
            agents_.erase(agent_result.first);
            error = "Principal registration failed";
            return false;
        }
    } catch (...) {
        agents_.erase(agent_result.first);
        throw;
    }

    return true;
}

bool AgentRegistry::unregister_agent(
    const AgentIdentity& identity,
    std::string& error
)
{
    error.clear();

    if (!valid_identifier(identity.agent_id) ||
        !valid_identifier(identity.instance_id)) {
        error = "Invalid agent identity";
        return false;
    }

    const IdentityKey key = make_key(identity);

    std::lock_guard<std::mutex> lock(mutex_);

    const auto agent_it = agents_.find(key);

    if (agent_it == agents_.end()) {
        error = "Agent instance is not registered";
        return false;
    }

    const std::string principal_id =
        agent_it->second.principal_id;

    principals_.erase(principal_id);
    agents_.erase(agent_it);

    return true;
}

bool AgentRegistry::resolve(
    const AgentIdentity& identity,
    AgentRegistration& registration
) const
{
    if (!valid_identifier(identity.agent_id) ||
        !valid_identifier(identity.instance_id)) {
        return false;
    }

    const IdentityKey key = make_key(identity);

    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = agents_.find(key);

    if (it == agents_.end()) {
        return false;
    }

    registration = it->second;
    return true;
}

bool AgentRegistry::resolve_principal(
    const std::string& principal_id,
    AgentRegistration& registration
) const
{
    if (!valid_identifier(principal_id)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const auto principal_it = principals_.find(principal_id);

    if (principal_it == principals_.end()) {
        return false;
    }

    const auto agent_it = agents_.find(principal_it->second);

    if (agent_it == agents_.end()) {
        return false;
    }

    registration = agent_it->second;
    return true;
}

bool AgentRegistry::set_active(
    const AgentIdentity& identity,
    bool active,
    std::string& error
)
{
    error.clear();

    if (!valid_identifier(identity.agent_id) ||
        !valid_identifier(identity.instance_id)) {
        error = "Invalid agent identity";
        return false;
    }

    const IdentityKey key = make_key(identity);

    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = agents_.find(key);

    if (it == agents_.end()) {
        error = "Agent instance is not registered";
        return false;
    }

    it->second.active = active;
    return true;
}

std::size_t AgentRegistry::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return agents_.size();
}

} // namespace mcp
