#ifndef MCP_IPC_TRANSPORT_HPP
#define MCP_IPC_TRANSPORT_HPP

#include "mcp/plugin_manager.hpp"
#include "mcp/authorization.hpp"
#include <csignal>
#include <string>
#include <sys/types.h>

namespace mcp {

class IpcServer {
public:
    IpcServer(PluginManager& plugin_manager, std::string socket_path);
    IpcServer(
        PluginManager& plugin_manager,
        std::string socket_path,
        AuthorizationPolicy authorization_policy
    );

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    int run(
        const volatile std::sig_atomic_t* shutdown_flag = nullptr
    );

private:
    PluginManager& plugin_manager_;
    AuthorizationPolicy authorization_policy_;
    std::string socket_path_;

    static constexpr unsigned long MAX_MESSAGE_SIZE =
    16ul * 1024ul * 1024ul;

    struct SocketIdentity {
        dev_t device = 0;
        ino_t inode = 0;
        bool valid = false;
    };

    int create_listener(SocketIdentity& identity);

    int handle_client(
        int client_fd,
        const PeerIdentity& peer_identity,
        const volatile std::sig_atomic_t* shutdown_flag
    );
    bool send_response(
        int client_fd,
        const std::string& response,
        const volatile std::sig_atomic_t* shutdown_flag
    );
};

} // namespace mcp

#endif
