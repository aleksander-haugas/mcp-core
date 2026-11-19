
#ifndef MCP_IPC_CLIENT_HPP
#define MCP_IPC_CLIENT_HPP

#include <string>

namespace mcp {

class IpcClient {
public:
    explicit IpcClient(std::string socket_path);
    ~IpcClient();

    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;

    bool connect(std::string& error);

    bool send_message(
        const std::string& message,
        std::string& error
    );

    bool receive_response(
        std::string& response,
        std::string& error
    );

    void disconnect();

    bool connected() const;

private:
    static constexpr std::size_t MAX_MESSAGE_SIZE =
        16u * 1024u * 1024u;

    std::string socket_path_;
    std::string receive_buffer_;
    int socket_fd_ = -1;

    bool send_all(
        const std::string& data,
        std::string& error
    );

    bool receive_line(
        std::string& line,
        std::string& error
    );
};

} // namespace mcp

#endif
