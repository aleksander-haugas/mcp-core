
#ifndef MCP_STDIO_BRIDGE_HPP
#define MCP_STDIO_BRIDGE_HPP

#include "mcp/ipc_client.hpp"

#include <cstddef>
#include <iosfwd>

namespace mcp {

class StdioBridge {
public:
    explicit StdioBridge(IpcClient& ipc_client);

    StdioBridge(const StdioBridge&) = delete;
    StdioBridge& operator=(const StdioBridge&) = delete;

    int run(
        std::istream& input,
        std::ostream& output,
        std::ostream& diagnostics
    );

private:
    IpcClient& ipc_client_;

    static constexpr std::size_t MAX_MESSAGE_SIZE =
        16u * 1024u * 1024u;
};

} // namespace mcp

#endif
