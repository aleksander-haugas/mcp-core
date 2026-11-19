
#include "mcp/stdio_bridge.hpp"
#include "mcp/json_rpc.hpp"

#include <istream>
#include <ostream>
#include <string>

namespace mcp {

StdioBridge::StdioBridge(IpcClient& ipc_client)
    : ipc_client_(ipc_client)
{
}

int StdioBridge::run(
    std::istream& input,
    std::ostream& output,
    std::ostream& diagnostics
)
{
    if (!ipc_client_.connected()) {
        diagnostics
            << "[MCP][BRIDGE][ERROR] IPC client is not connected\n";

        diagnostics.flush();
        return 1;
    }

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

            if (message.size() >= MAX_MESSAGE_SIZE) {
                oversized = true;
                break;
            }

            message.push_back(character);
        }

        if (oversized) {
            diagnostics
                << "[MCP][BRIDGE][ERROR] Message exceeds "
                << MAX_MESSAGE_SIZE
                << " bytes\n";

            diagnostics.flush();
            return 1;
        }

        if (!received_data) {
            if (input.bad()) {
                diagnostics
                    << "[MCP][BRIDGE][ERROR] Input stream failure\n";

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
        if (message.empty() && !newline_received) {
            break;
        }

        std::string error;

        if (!ipc_client_.send_message(message, error)) {
            diagnostics
                << "[MCP][BRIDGE][ERROR] IPC send failed: "
                << error << '\n';

            diagnostics.flush();
            return 1;
        }

        // Valid JSON-RPC notifications have no response.
        // Invalid requests are still forwarded and may receive
        // a JSON-RPC error response from the Core.
        JsonRpcRequest request;

        std::string parse_error;

        const bool valid_request =
            jsonrpc::parse_request(
                message,
                request,
                parse_error
            );

        if (valid_request && request.is_notification()) {
            if (!newline_received) {
                break;
            }

            continue;
        }

        std::string response;

        if (!ipc_client_.receive_response(response, error)) {
            diagnostics
                << "[MCP][BRIDGE][ERROR] IPC receive failed: "
                << error << '\n';

            diagnostics.flush();
            return 1;
        }

        output << response << '\n';
        output.flush();

        if (!output.good()) {
            diagnostics
                << "[MCP][BRIDGE][ERROR] Output stream failure\n";

            diagnostics.flush();
            return 1;
        }

        if (!newline_received) {
            break;
        }
    }

    diagnostics << "[MCP][BRIDGE] Input stream closed\n";
    diagnostics.flush();

    return 0;
}

} // namespace mcp
