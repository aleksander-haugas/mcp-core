
#include "mcp/ipc_client.hpp"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cerrno>
#include <cstring>

#include <unistd.h>

namespace mcp {

IpcClient::IpcClient(std::string socket_path)
    : socket_path_(std::move(socket_path))
{
}

IpcClient::~IpcClient()
{
    disconnect();
}

bool IpcClient::connect(std::string& error)
{
    if (socket_fd_ >= 0) {
        error = "IPC client is already connected";
        return false;
    }

    if (socket_path_.empty() ||
        socket_path_.size() >= sizeof(sockaddr_un::sun_path)) {
        error = "Invalid Unix socket path";
        return false;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) {
        error = std::string("socket(): ") + std::strerror(errno);
        return false;
    }

    int no_sigpipe = 1;

    if (setsockopt(
            fd,
            SOL_SOCKET,
            SO_NOSIGPIPE,
            &no_sigpipe,
            sizeof(no_sigpipe)
        ) < 0) {
        error = std::string("SO_NOSIGPIPE: ") +
                std::strerror(errno);

        close(fd);
        return false;
    }

    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;

    std::memcpy(
        address.sun_path,
        socket_path_.c_str(),
        socket_path_.size() + 1
    );

    if (::connect(
            fd,
            reinterpret_cast<struct sockaddr*>(&address),
            sizeof(address)
        ) < 0) {
        error = std::string("connect(): ") +
                std::strerror(errno);

        close(fd);
        return false;
    }

    socket_fd_ = fd;
    receive_buffer_.clear();

    return true;
}

bool IpcClient::send_all(
    const std::string& data,
    std::string& error
)
{
    std::size_t sent = 0;

    while (sent < data.size()) {
        const ssize_t result = send(
            socket_fd_,
            data.data() + sent,
            data.size() - sent,
            0
        );

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            error = std::string("send(): ") +
                    std::strerror(errno);
            return false;
        }

        if (result == 0) {
            error = "Socket closed while sending";
            return false;
        }

        sent += static_cast<std::size_t>(result);
    }

    return true;
}

bool IpcClient::send_message(
    const std::string& message,
    std::string& error
)
{
    if (socket_fd_ < 0) {
        error = "IPC client is not connected";
        return false;
    }

    if (message.size() > MAX_MESSAGE_SIZE) {
        error = "IPC message exceeds size limit";
        return false;
    }

    if (message.find('\n') != std::string::npos) {
        error = "IPC message contains an embedded newline";
        return false;
    }

    std::string framed_message = message;
    framed_message.push_back('\n');

    return send_all(framed_message, error);
}

bool IpcClient::receive_line(
    std::string& line,
    std::string& error
)
{
    line.clear();

    while (true) {
        const std::size_t newline =
            receive_buffer_.find('\n');

        if (newline != std::string::npos) {
            if (newline > MAX_MESSAGE_SIZE) {
                error = "IPC response exceeds size limit";
                return false;
            }

            line.assign(receive_buffer_, 0, newline);
            receive_buffer_.erase(0, newline + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            return true;
        }

        if (receive_buffer_.size() > MAX_MESSAGE_SIZE) {
            error = "IPC response exceeds size limit";
            return false;
        }

        char chunk[8192];

        const ssize_t received = recv(
            socket_fd_,
            chunk,
            sizeof(chunk),
            0
        );

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }

            error = std::string("recv(): ") +
                    std::strerror(errno);
            return false;
        }

        if (received == 0) {
            error = "IPC server disconnected";
            return false;
        }

        receive_buffer_.append(
            chunk,
            static_cast<std::size_t>(received)
        );
    }
}

bool IpcClient::receive_response(
    std::string& response,
    std::string& error
)
{
    if (socket_fd_ < 0) {
        error = "IPC client is not connected";
        return false;
    }

    return receive_line(response, error);
}

void IpcClient::disconnect()
{
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }

    receive_buffer_.clear();
}

bool IpcClient::connected() const
{
    return socket_fd_ >= 0;
}

} // namespace mcp
