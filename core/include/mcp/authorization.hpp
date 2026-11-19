#ifndef MCP_AUTHORIZATION_HPP
#define MCP_AUTHORIZATION_HPP

#include <sys/types.h>

#include <string>
#include <vector>

namespace mcp {

struct PeerIdentity {
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
    bool authenticated = false;
};

struct CapabilityGrant {
    uid_t uid;
    std::vector<std::string> capabilities;
};

class AuthorizationPolicy {
public:
    explicit AuthorizationPolicy(
        std::vector<CapabilityGrant> grants
    );

    static AuthorizationPolicy default_local_policy();

    bool authorize(
        const PeerIdentity& identity,
        const std::string& tool_name,
        const std::vector<std::string>& required_capabilities,
        std::string& denial_reason
    ) const;

private:
    const std::vector<CapabilityGrant> grants_;
};

} // namespace mcp

#endif
