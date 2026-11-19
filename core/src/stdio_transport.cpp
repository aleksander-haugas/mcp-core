#include "mcp/stdio_transport.hpp"

#include <istream>
#include <ostream>
#include <string>

namespace mcp {

StdioTransport::StdioTransport(RequestHandler& request_handler)
    : request_handler_(request_handler)
{
}

int StdioTransport::run(
    std::istream& input,
    std::ostream& output,
    std::ostream& diagnostics
)
{
    std::string message;
    message.reserve(4096);

    while (true) {
        message.clear();

        bool received_data = false;
        bool oversized = false;
        bool newline_received = false;

        char character = '\0';

        while (input.get(character)) {
            received_data = true;

            if (character == '\n') {
                newline_received = true;
                break;
            }

            if (!oversized) {
                if (message.size() >= MAX_MESSAGE_SIZE) {
                    oversized = true;
                    message.clear();
                } else {
                    message.push_back(character);
                }
            }
        }

        if (oversized) {
            diagnostics
                << "[MCP][STDIO][ERROR] Message exceeds "
                << MAX_MESSAGE_SIZE
                << " bytes\n";

            diagnostics.flush();
            return 1;
        }

        if (!received_data) {
            if (input.bad()) {
                diagnostics
                    << "[MCP][STDIO][ERROR] Input stream failure\n";

                diagnostics.flush();
                return 1;
            }

            break;
        }

        // Accept CRLF as well as LF.
        if (!message.empty() && message.back() == '\r') {
            message.pop_back();
        }

        // Process a final message even if EOF arrives without LF.
        if (message.empty()) {
            if (!newline_received) {
                break;
            }

            // An empty line is not a valid JSON-RPC message.
            // Pass it to the handler so it can report a parse error.
        }

        std::string response;

        try {
            response = request_handler_.handle(message);
        } catch (const std::exception& exception) {
            diagnostics
                << "[MCP][STDIO][ERROR] Request handler exception: "
                << exception.what()
                << '\n';

            diagnostics.flush();
            return 1;
        } catch (...) {
            diagnostics
                << "[MCP][STDIO][ERROR] Unknown request handler exception\n";

            diagnostics.flush();
            return 1;
        }

        // Notifications do not produce a response.
        if (!response.empty()) {
            output << response << '\n';
            output.flush();

            if (!output.good()) {
                diagnostics
                    << "[MCP][STDIO][ERROR] Output stream failure\n";

                diagnostics.flush();
                return 1;
            }
        }

        if (!newline_received) {
            break;
        }
    }

    diagnostics << "[MCP][STDIO] Input stream closed\n";
    diagnostics.flush();

    return 0;
}

} // namespace mcp
