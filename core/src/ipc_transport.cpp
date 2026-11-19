
#include "mcp/ipc_transport.hpp"
#include "mcp/request_handler.hpp"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <fcntl.h>
#include <poll.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

#include <unistd.h>

namespace mcp {

    namespace {

        constexpr int POLL_INTERVAL_MS = 500;
        constexpr int LISTEN_BACKLOG = 8;

        bool shutdown_requested(
            const volatile std::sig_atomic_t* flag
        )
        {
            return flag != nullptr && *flag != 0;
        }

        bool set_nonblocking(int fd)
        {
            const int flags = fcntl(fd, F_GETFL, 0);

            if (flags < 0) {
                return false;
            }

            return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
        }

        bool socket_matches_identity(
            const std::string& path,
            dev_t device,
            ino_t inode
        )
        {
            struct stat current {};

            return lstat(path.c_str(), &current) == 0 &&
            S_ISSOCK(current.st_mode) &&
            current.st_dev == device &&
            current.st_ino == inode;
        }

        bool unlink_socket_if_matches(
            const std::string& path,
            dev_t device,
            ino_t inode
        )
        {
            if (!socket_matches_identity(path, device, inode)) {
                struct stat current {};

                if (lstat(path.c_str(), &current) < 0 &&
                    errno == ENOENT) {
                    return true;
                    }

                    std::cerr
                    << "[MCP][IPC][WARNING] "
                    << "Socket path changed; refusing to unlink: "
                    << path << '\n';

                return false;
            }

            if (unlink(path.c_str()) < 0) {
                std::cerr
                << "[MCP][IPC][ERROR] "
                << "Socket unlink failed: "
                << std::strerror(errno) << '\n';

                return false;
            }

            return true;
        }

        int wait_for_event(
            int fd,
            short events,
            const volatile std::sig_atomic_t* shutdown_flag
        )
        {
            struct pollfd descriptor {};
            descriptor.fd = fd;
            descriptor.events = events;

            while (!shutdown_requested(shutdown_flag)) {
                const int result = poll(
                    &descriptor,
                    1,
                    POLL_INTERVAL_MS
                );

                if (result < 0) {
                    if (errno == EINTR) {
                        if (shutdown_requested(shutdown_flag)) {
                            return 0;
                        }

                        // Sin indicador de apagado, conservar la semántica
                        // tradicional: una señal interrumpe la espera.
                        if (shutdown_flag == nullptr) {
                            return 0;
                        }

                        continue;
                    }

                    return -1;
                }

                if (result == 0) {
                    continue;
                }

                if (descriptor.revents & POLLNVAL) {
                    errno = EBADF;
                    return -1;
                }

                if (descriptor.revents & (events | POLLERR | POLLHUP)) {
                    return descriptor.revents;
                }
            }

            return 0;
        }

    } // namespace

    IpcServer::IpcServer(
        PluginManager& plugin_manager,
        std::string socket_path
    )
    : IpcServer(
        plugin_manager,
        std::move(socket_path),
                AuthorizationPolicy::default_local_policy()
    )
    {
    }

    IpcServer::IpcServer(
        PluginManager& plugin_manager,
        std::string socket_path,
        AuthorizationPolicy authorization_policy
    )
    : plugin_manager_(plugin_manager),
    authorization_policy_(std::move(authorization_policy)),
    socket_path_(std::move(socket_path))
    {
    }

    int IpcServer::create_listener(SocketIdentity& identity)
    {
        identity = {};

        if (socket_path_.empty() ||
            socket_path_.size() >= sizeof(sockaddr_un::sun_path) ||
            socket_path_.front() != '/' ||
            socket_path_.back() == '/') {
            std::cerr
            << "[MCP][IPC][ERROR] Invalid socket path\n";
        return -1;
            }

            const std::size_t separator =
            socket_path_.find_last_of('/');

            const std::string parent =
            separator == 0
            ? "/"
            : socket_path_.substr(0, separator);

            struct stat parent_info {};

            if (lstat(parent.c_str(), &parent_info) < 0) {
                std::cerr
                << "[MCP][IPC][ERROR] Cannot inspect socket directory: "
                << std::strerror(errno) << '\n';
                return -1;
            }

            if (!S_ISDIR(parent_info.st_mode) ||
                parent_info.st_uid != geteuid() ||
                (parent_info.st_mode & 0022) != 0) {
                std::cerr
                << "[MCP][IPC][ERROR] "
                << "Untrusted socket directory: "
                << parent << '\n';
            return -1;
                }

                if ((parent_info.st_mode & S_IWUSR) == 0 ||
                    (parent_info.st_mode & S_IXUSR) == 0) {
                    std::cerr
                    << "[MCP][IPC][ERROR] "
                    << "Socket directory is not writable/searchable "
                    << "by its owner\n";
                return -1;
                    }

                    struct stat existing {};

                    if (lstat(socket_path_.c_str(), &existing) == 0) {
                        std::cerr
                        << "[MCP][IPC][ERROR] Socket path already exists: "
                        << socket_path_ << '\n';
                        return -1;
                    }

                    if (errno != ENOENT) {
                        std::cerr
                        << "[MCP][IPC][ERROR] Cannot inspect socket path: "
                        << std::strerror(errno) << '\n';
                        return -1;
                    }

                    const int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

                    if (server_fd < 0) {
                        std::cerr
                        << "[MCP][IPC][ERROR] socket(): "
                        << std::strerror(errno) << '\n';
                        return -1;
                    }

                    if (!set_nonblocking(server_fd)) {
                        std::cerr
                        << "[MCP][IPC][ERROR] Cannot set listener nonblocking: "
                        << std::strerror(errno) << '\n';

                        close(server_fd);
                        return -1;
                    }

                    struct sockaddr_un address {};
                    address.sun_family = AF_UNIX;

                    std::memcpy(
                        address.sun_path,
                        socket_path_.c_str(),
                                socket_path_.size() + 1
                    );

                    if (bind(
                        server_fd,
                        reinterpret_cast<struct sockaddr*>(&address),
                             sizeof(address)
                    ) < 0) {
                        std::cerr
                        << "[MCP][IPC][ERROR] bind(): "
                        << std::strerror(errno) << '\n';

                        close(server_fd);
                        return -1;
                    }

                    struct stat created {};

                    if (lstat(socket_path_.c_str(), &created) < 0 ||
                        !S_ISSOCK(created.st_mode)) {
                        std::cerr
                        << "[MCP][IPC][ERROR] "
                        << "Cannot identify socket immediately after bind\n";

                    close(server_fd);
                    return -1;
                        }

                        identity.device = created.st_dev;
                        identity.inode = created.st_ino;
                        identity.valid = true;

                        if (chmod(socket_path_.c_str(), 0660) < 0) {
                            std::cerr
                            << "[MCP][IPC][ERROR] chmod(): "
                            << std::strerror(errno) << '\n';

                            unlink_socket_if_matches(
                                socket_path_,
                                identity.device,
                                identity.inode
                            );

                            close(server_fd);
                            identity.valid = false;
                            return -1;
                        }

                        if (!socket_matches_identity(
                            socket_path_,
                            identity.device,
                            identity.inode
                        )) {
                            std::cerr
                            << "[MCP][IPC][ERROR] "
                            << "Socket identity changed during creation\n";

                            unlink_socket_if_matches(
                                socket_path_,
                                identity.device,
                                identity.inode
                            );

                            close(server_fd);
                            return -1;
                        }

                        if (listen(server_fd, LISTEN_BACKLOG) < 0) {
                            std::cerr
                            << "[MCP][IPC][ERROR] listen(): "
                            << std::strerror(errno) << '\n';

                            unlink_socket_if_matches(
                                socket_path_,
                                identity.device,
                                identity.inode
                            );

                            close(server_fd);
                            identity.valid = false;
                            return -1;
                        }

                        std::cerr
                        << "[MCP][IPC] Listening on "
                        << socket_path_ << '\n';

                        return server_fd;
    }

    bool IpcServer::send_response(
        int client_fd,
        const std::string& response,
        const volatile std::sig_atomic_t* shutdown_flag
    )
    {
        if (response.size() > MAX_MESSAGE_SIZE) {
            std::cerr
            << "[MCP][IPC][ERROR] Response exceeds size limit\n";
            return false;
        }

        std::string framed_response = response;
        framed_response.push_back('\n');

        std::size_t sent = 0;

        while (sent < framed_response.size()) {
            if (shutdown_requested(shutdown_flag)) {
                return false;
            }

            const ssize_t result = send(
                client_fd,
                framed_response.data() + sent,
                                        framed_response.size() - sent,
                                        0
            );

            if (result > 0) {
                sent += static_cast<std::size_t>(result);
                continue;
            }

            if (result == 0) {
                return false;
            }

            if (errno == EINTR) {
                continue;
            }

            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                return false;
            }

            const int ready = wait_for_event(
                client_fd,
                POLLOUT,
                shutdown_flag
            );

            if (ready <= 0 ||
                (ready & (POLLERR | POLLHUP | POLLNVAL))) {
                return false;
                }
        }

        return true;
    }

    int IpcServer::handle_client(
        int client_fd,
        const PeerIdentity& peer_identity,
        const volatile std::sig_atomic_t* shutdown_flag
    )
    {
        RequestHandler handler(
            plugin_manager_,
            authorization_policy_,
            peer_identity
        );

        std::string buffer;
        buffer.reserve(4096);

        char chunk[8192];

        while (!shutdown_requested(shutdown_flag)) {
            const int ready = wait_for_event(
                client_fd,
                POLLIN,
                shutdown_flag
            );

            if (ready == 0) {
                break;
            }

            if (ready < 0) {
                std::cerr
                << "[MCP][IPC][ERROR] poll(): "
                << std::strerror(errno) << '\n';
                return 1;
            }

            bool peer_closed = false;

            while (true) {
                const ssize_t received = recv(
                    client_fd,
                    chunk,
                    sizeof(chunk),
                                              0
                );

                if (received > 0) {
                    buffer.append(
                        chunk,
                        static_cast<std::size_t>(received)
                    );

                    while (true) {
                        const std::size_t newline =
                        buffer.find('\n');

                        if (newline == std::string::npos) {
                            break;
                        }

                        if (newline > MAX_MESSAGE_SIZE) {
                            std::cerr
                            << "[MCP][IPC][ERROR] "
                            << "Message exceeds size limit\n";
                            return 1;
                        }

                        std::string message =
                        buffer.substr(0, newline);

                        buffer.erase(0, newline + 1);

                        if (!message.empty() &&
                            message.back() == '\r') {
                            message.pop_back();
                            }

                            if (shutdown_requested(shutdown_flag)) {
                                return 0;
                            }

                            std::string response;

                        try {
                            response = handler.handle(message);
                        } catch (const std::exception& exception) {
                            std::cerr
                            << "[MCP][IPC][ERROR] "
                            << "Handler exception: "
                            << exception.what() << '\n';
                            return 1;
                        } catch (...) {
                            std::cerr
                            << "[MCP][IPC][ERROR] "
                            << "Unknown handler exception\n";
                            return 1;
                        }

                        if (!response.empty() &&
                            !send_response(
                                client_fd,
                                response,
                                shutdown_flag
                            )) {
                            if (shutdown_requested(shutdown_flag)) {
                                return 0;
                            }

                            std::cerr
                            << "[MCP][IPC][ERROR] "
                            << "Failed to send response\n";
                            return 1;
                            }

                            if (buffer.size() > MAX_MESSAGE_SIZE) {
                                std::cerr
                                << "[MCP][IPC][ERROR] "
                                << "Message exceeds size limit\n";
                                return 1;
                            }
                    }

                    if (buffer.size() > MAX_MESSAGE_SIZE) {
                        std::cerr
                        << "[MCP][IPC][ERROR] "
                        << "Message exceeds size limit\n";
                        return 1;
                    }

                    continue;
                }

                if (received == 0) {
                    peer_closed = true;
                    break;
                }

                if (errno == EINTR) {
                    if (shutdown_requested(shutdown_flag)) {
                        return 0;
                    }

                    continue;
                }

                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }

                std::cerr
                << "[MCP][IPC][ERROR] recv(): "
                << std::strerror(errno) << '\n';

                return 1;
            }

            if (peer_closed) {
                break;
            }
        }

        if (shutdown_requested(shutdown_flag)) {
            return 0;
        }

        // Process a final message without a trailing newline.
        if (!buffer.empty()) {
            std::string response;

            try {
                response = handler.handle(buffer);
            } catch (const std::exception& exception) {
                std::cerr
                << "[MCP][IPC][ERROR] "
                << "Final request handler exception: "
                << exception.what() << '\n';
                return 1;
            } catch (...) {
                std::cerr
                << "[MCP][IPC][ERROR] "
                << "Unknown final request handler exception\n";
                return 1;
            }

            if (!response.empty() &&
                !send_response(
                    client_fd,
                    response,
                    shutdown_flag
                )) {
                return shutdown_requested(shutdown_flag) ? 0 : 1;
                }
        }

        std::cerr << "[MCP][IPC] Client disconnected\n";
        return 0;
    }

    int IpcServer::run(
        const volatile std::sig_atomic_t* shutdown_flag
    )
    {
        SocketIdentity socket_identity;

        const int server_fd = create_listener(socket_identity);

        if (server_fd < 0) {
            return 1;
        }

            int exit_code = 0;

            while (!shutdown_requested(shutdown_flag)) {
                const int ready = wait_for_event(
                    server_fd,
                    POLLIN,
                    shutdown_flag
                );

                if (ready == 0) {
                    break;
                }

                if (ready < 0) {
                    std::cerr
                    << "[MCP][IPC][ERROR] listener poll(): "
                    << std::strerror(errno) << '\n';

                    exit_code = 1;
                    break;
                }

                if (ready & (POLLERR | POLLHUP | POLLNVAL)) {
                    std::cerr
                    << "[MCP][IPC][ERROR] "
                    << "Listener reported an error\n";

                    exit_code = 1;
                    break;
                }

                while (!shutdown_requested(shutdown_flag)) {
                    const int client_fd = accept(
                        server_fd,
                        nullptr,
                        nullptr
                    );

                    if (client_fd < 0) {
                        if (errno == EINTR) {
                            continue;
                        }

                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }

                        std::cerr
                        << "[MCP][IPC][ERROR] accept(): "
                        << std::strerror(errno) << '\n';

                        exit_code = 1;
                        break;
                    }

                    int no_sigpipe = 1;

                    if (setsockopt(
                        client_fd,
                        SOL_SOCKET,
                        SO_NOSIGPIPE,
                        &no_sigpipe,
                        sizeof(no_sigpipe)
                    ) < 0 ||
                    !set_nonblocking(client_fd)) {

                        std::cerr
                        << "[MCP][IPC][ERROR] "
                        << "Cannot configure client socket: "
                        << std::strerror(errno) << '\n';

                        close(client_fd);
                        continue;
                    }

                    uid_t peer_uid = static_cast<uid_t>(-1);
                    gid_t peer_gid = static_cast<gid_t>(-1);

                    if (getpeereid(client_fd, &peer_uid, &peer_gid) != 0) {
                        std::cerr
                        << "[MCP][IPC][SECURITY] "
                        << "Cannot authenticate IPC client: "
                        << std::strerror(errno)
                        << '\n';

                        close(client_fd);
                        continue;
                    }

                    const PeerIdentity peer_identity{
                        peer_uid,
                        peer_gid,
                        true
                    };

                    std::cerr
                    << "[MCP][IPC][AUTH] "
                    << "Authenticated client uid=" << peer_uid
                    << " gid=" << peer_gid
                    << '\n';

                    const int client_result = handle_client(
                        client_fd,
                        peer_identity,
                        shutdown_flag
                    );

                    close(client_fd);

                    if (client_result != 0) {
                        std::cerr
                        << "[MCP][IPC] "
                        << "Client connection ended with errors\n";
                    }

                    if (shutdown_requested(shutdown_flag)) {
                        break;
                    }
                }

                if (exit_code != 0) {
                    break;
                }
            }

            close(server_fd);

            if (socket_identity.valid &&
                !unlink_socket_if_matches(
                    socket_path_,
                    socket_identity.device,
                    socket_identity.inode
                )) {
                exit_code = 1;
                }

                std::cerr << "[MCP][IPC] Server stopped\n";

                return exit_code;
    }

} // namespace mcp
