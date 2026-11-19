#include "mcp/authorization.hpp"

#include <algorithm>
#include <iostream>
#include <utility>

namespace mcp {

AuthorizationPolicy::AuthorizationPolicy(
    std::vector<CapabilityGrant> grants
)
    : grants_(std::move(grants))
{
}

AuthorizationPolicy AuthorizationPolicy::default_local_policy()
{
    std::vector<CapabilityGrant> grants;

    grants.push_back({
        static_cast<uid_t>(1001),
        {
            "system.read",
            "hardware.read",
            "nakama.read",
            "nakama.control"
        }
    });

    grants.push_back({
        static_cast<uid_t>(1003),
        {
            "system.read",
            "hardware.read",
            "nakama.read",
            "nakama.control"
        }
    });

    return AuthorizationPolicy(std::move(grants));
}

bool AuthorizationPolicy::authorize(
    const PeerIdentity& identity,
    const std::string& tool_name,
    const std::vector<std::string>& required_capabilities,
    std::string& denial_reason
) const
{
    denial_reason.clear();

    if (!identity.authenticated) {
        denial_reason = "Unauthenticated client";

        std::cerr
            << "[MCP][AUTH][DENIED] "
            << "Unauthenticated client"
            << " tool=" << tool_name
            << '\n';

        return false;
    }

    if (required_capabilities.empty()) {
        denial_reason = "Tool has no declared capabilities";

        std::cerr
            << "[MCP][AUTH][DENIED] "
            << "uid=" << identity.uid
            << " gid=" << identity.gid
            << " tool=" << tool_name
            << " reason=no declared capabilities"
            << '\n';

        return false;
    }

    const auto grant = std::find_if(
        grants_.begin(),
        grants_.end(),
        [&identity](const CapabilityGrant& candidate) {
            return candidate.uid == identity.uid;
        }
    );

    if (grant == grants_.end()) {
        denial_reason = "Client has no authorization grant";

        std::cerr
            << "[MCP][AUTH][DENIED] "
            << "uid=" << identity.uid
            << " gid=" << identity.gid
            << " tool=" << tool_name
            << " reason=no UID grant"
            << '\n';

        return false;
    }

    for (const std::string& required : required_capabilities) {
        const bool permitted = std::find(
            grant->capabilities.begin(),
            grant->capabilities.end(),
            required
        ) != grant->capabilities.end();

        if (!permitted) {
            denial_reason = "Required capability not granted";

            std::cerr
                << "[MCP][AUTH][DENIED] "
                << "uid=" << identity.uid
                << " gid=" << identity.gid
                << " tool=" << tool_name
                << " capability=" << required
                << '\n';

            return false;
        }
    }

    std::cerr
        << "[MCP][AUTH][GRANTED] "
        << "uid=" << identity.uid
        << " gid=" << identity.gid
        << " tool=" << tool_name
        << '\n';

    return true;
}

} // namespace mcp
