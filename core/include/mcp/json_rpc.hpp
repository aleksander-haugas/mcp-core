
#ifndef MCP_JSON_RPC_HPP
#define MCP_JSON_RPC_HPP

#include <string>

namespace mcp {

struct JsonRpcRequest {
    std::string method;
    std::string id_json;
    std::string params_json;

    bool has_id = false;
    bool has_params = false;

    bool is_notification() const
    {
        return !has_id;
    }
};

namespace jsonrpc {

constexpr int PARSE_ERROR = -32700;
constexpr int INVALID_REQUEST = -32600;
constexpr int METHOD_NOT_FOUND = -32601;
constexpr int INVALID_PARAMS = -32602;
constexpr int INTERNAL_ERROR = -32603;

bool parse_request(
    const std::string& input,
    JsonRpcRequest& request,
    std::string& error
);

std::string make_result(
    const std::string& id_json,
    const std::string& result_json
);

std::string make_error(
    const std::string& id_json,
    int code,
    const std::string& message,
    const std::string& data_json = ""
);

} // namespace jsonrpc
} // namespace mcp

#endif
