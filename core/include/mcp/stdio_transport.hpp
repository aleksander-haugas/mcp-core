#ifndef MCP_STDIO_TRANSPORT_HPP
#define MCP_STDIO_TRANSPORT_HPP

#include "mcp/request_handler.hpp"

#include <iosfwd>

namespace mcp {

class StdioTransport {
public:
    explicit StdioTransport(RequestHandler& request_handler);

    StdioTransport(const StdioTransport&) = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;

    int run(
        std::istream& input,
        std::ostream& output,
        std::ostream& diagnostics
    );

private:
    RequestHandler& request_handler_;

    static constexpr std::size_t MAX_MESSAGE_SIZE =
        16u * 1024u * 1024u;
};

} // namespace mcp

#endif
