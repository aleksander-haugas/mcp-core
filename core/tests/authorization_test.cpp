#include "mcp/authorization.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const char* description)
{
    ++checks;

    if (condition) {
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++failures;
        std::cerr << "[FAIL] " << description << '\n';
    }
}

} // namespace

int main()
{
    using namespace mcp;

    const AuthorizationPolicy policy =
        AuthorizationPolicy::default_local_policy();

    const PeerIdentity warbotz{
        static_cast<uid_t>(1001),
        static_cast<gid_t>(1001),
        true
    };

    const PeerIdentity daemon{
        static_cast<uid_t>(1003),
        static_cast<gid_t>(1004),
        true
    };

    const PeerIdentity unauthorized{
        static_cast<uid_t>(2000),
        static_cast<gid_t>(2000),
        true
    };

    const PeerIdentity unauthenticated{
        static_cast<uid_t>(1001),
        static_cast<gid_t>(1001),
        false
    };

    std::string reason;

    check(
        policy.authorize(
            warbotz, "system_info",
            {"system.read"}, reason
        ),
        "warbotz can read system information"
    );

    check(
        policy.authorize(
            warbotz, "hardware_gpu_info",
            {"hardware.read"}, reason
        ),
        "warbotz can read hardware information"
    );

    check(
        policy.authorize(
            daemon, "system_info",
            {"system.read"}, reason
        ),
        "mcp daemon can read system information"
    );

    check(
        policy.authorize(
            daemon, "hardware_gpu_info",
            {"hardware.read"}, reason
        ),
        "mcp daemon can read hardware information"
    );

    check(
        policy.authorize(
            warbotz, "nakama_status",
            {"nakama.read"}, reason
        ),
        "warbotz can read Nakama status"
    );

    check(
        policy.authorize(
            warbotz, "nakama_disconnect",
            {"nakama.control"}, reason
        ),
        "warbotz can control Nakama connection"
    );

    check(
        policy.authorize(
            daemon, "nakama_status",
            {"nakama.read"}, reason
        ),
        "mcp daemon can read Nakama status"
    );

    check(
        policy.authorize(
            daemon, "nakama_disconnect",
            {"nakama.control"}, reason
        ),
        "mcp daemon can control Nakama connection"
    );

    check(
        !policy.authorize(
            unauthorized, "nakama_disconnect",
            {"nakama.control"}, reason
        ),
        "unknown UID cannot control Nakama"
    );

    check(
        !policy.authorize(
            unauthorized, "system_info",
            {"system.read"}, reason
        ),
        "unknown UID is denied"
    );

    check(
        !policy.authorize(
            unauthenticated, "system_info",
            {"system.read"}, reason
        ),
        "unauthenticated identity is denied"
    );

    check(
        !policy.authorize(
            warbotz, "dangerous_operation",
            {"system.write"}, reason
        ),
        "ungranted capability is denied"
    );

    check(
        !policy.authorize(
            warbotz, "undeclared_tool",
            {}, reason
        ),
        "tool without declared capabilities is denied"
    );

    check(
        !policy.authorize(
            warbotz, "mixed_operation",
            {"system.read", "system.write"}, reason
        ),
        "all required capabilities must be granted"
    );

    std::cout
        << "\nAuthorization tests: "
        << (checks - failures) << "/" << checks
        << " passed\n";

    return failures == 0 ? 0 : 1;
}
