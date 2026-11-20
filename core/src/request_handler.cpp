
#include "mcp/request_handler.hpp"
#include "mcp/json_rpc.hpp"

#include <json-c/json.h>

#include <memory>
#include <string>

namespace mcp {

namespace {

using JsonPtr = std::unique_ptr<json_object, decltype(&json_object_put)>;

constexpr const char* MCP_PROTOCOL_VERSION = "2025-11-25";
constexpr const char* SERVER_NAME = "mcp-core";
constexpr const char* SERVER_VERSION = "0.1.0";

JsonPtr parse_json(const std::string& input)
{
    if (input.empty()) {
        return JsonPtr(nullptr, json_object_put);
    }

    json_tokener* tokener = json_tokener_new();

    if (tokener == nullptr) {
        return JsonPtr(nullptr, json_object_put);
    }

    json_tokener_set_flags(
        tokener,
        JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8
    );

    std::string source = input;
    source.push_back(' ');

    json_object* parsed = json_tokener_parse_ex(
        tokener,
        source.data(),
        static_cast<int>(source.size())
    );

    const json_tokener_error status =
        json_tokener_get_error(tokener);

    const std::size_t end =
        json_tokener_get_parse_end(tokener);

    bool trailing_data = false;

    for (std::size_t i = end; i < source.size(); ++i) {
        const unsigned char ch =
            static_cast<unsigned char>(source[i]);

        if (ch != ' ' && ch != '\t' &&
            ch != '\n' && ch != '\r') {
            trailing_data = true;
            break;
        }
    }

    json_tokener_free(tokener);

    if (status != json_tokener_success || trailing_data) {
        json_object_put(parsed);
        return JsonPtr(nullptr, json_object_put);
    }

    return JsonPtr(parsed, json_object_put);
}

std::string serialize_json(json_object* object)
{
    if (object == nullptr) {
        return "null";
    }

    const char* value = json_object_to_json_string_ext(
        object,
        JSON_C_TO_STRING_PLAIN
    );

    return value != nullptr ? value : "";
}

bool add_string(
    json_object* object,
    const char* key,
    const std::string& value
)
{
    json_object* item = json_object_new_string_len(
        value.data(),
        static_cast<int>(value.size())
    );

    if (item == nullptr) {
        return false;
    }

    if (json_object_object_add(object, key, item) != 0) {
        json_object_put(item);
        return false;
    }

    return true;
}

bool add_object(
    json_object* parent,
    const char* key,
    JsonPtr& child
)
{
    json_object* value = child.release();

    if (json_object_object_add(parent, key, value) != 0) {
        json_object_put(value);
        return false;
    }

    return true;
}

bool add_array(
    json_object* parent,
    const char* key,
    JsonPtr& child
)
{
    json_object* value = child.release();

    if (json_object_object_add(parent, key, value) != 0) {
        json_object_put(value);
        return false;
    }

    return true;
}

bool get_string(
    json_object* object,
    const char* key,
    std::string& value
)
{
    json_object* item = nullptr;

    if (object == nullptr ||
        !json_object_object_get_ex(object, key, &item) ||
        item == nullptr ||
        json_object_get_type(item) != json_type_string) {
        return false;
    }

    const char* raw = json_object_get_string(item);

    if (raw == nullptr) {
        return false;
    }

    value.assign(
        raw,
        static_cast<std::size_t>(
            json_object_get_string_len(item)
        )
    );

    return !value.empty();
}

std::string invalid_params(
    const std::string& id,
    const std::string& message
)
{
    return jsonrpc::make_error(
        id,
        jsonrpc::INVALID_PARAMS,
        message
    );
}

} // namespace

RequestHandler::RequestHandler(
    PluginManager& plugin_manager,
    const AuthorizationPolicy& authorization_policy,
    PeerIdentity peer_identity
)
: plugin_manager_(plugin_manager),
authorization_policy_(authorization_policy),
peer_identity_(peer_identity)
{
}

bool RequestHandler::initialized() const
{
    return initialized_;
}

void RequestHandler::reset()
{
    initialize_response_sent_ = false;
    initialized_ = false;
}

std::string RequestHandler::handle(const std::string& input)
{
    JsonRpcRequest request;
    std::string parse_error;

    if (!jsonrpc::parse_request(input, request, parse_error)) {
        const int code =
            parse_error.rfind("Invalid JSON:", 0) == 0
                ? jsonrpc::PARSE_ERROR
                : jsonrpc::INVALID_REQUEST;

        return jsonrpc::make_error(
            "null",
            code,
            parse_error
        );
    }

    // Las notificaciones nunca reciben respuesta.
    if (request.is_notification()) {
        if (request.method == "notifications/initialized" &&
            initialize_response_sent_) {
            initialized_ = true;
            }

            return {};
    }

    if (request.method == "initialize") {
        if (initialize_response_sent_) {
            return jsonrpc::make_error(
                request.id_json,
                jsonrpc::INVALID_REQUEST,
                "MCP session is already initialized"
            );
        }

        return handle_initialize(
            request.id_json,
            request.params_json
        );
    }

    if (!initialized_) {
        return jsonrpc::make_error(
            request.id_json,
            jsonrpc::INVALID_REQUEST,
            "MCP session has not been initialized"
        );
    }

    if (request.method == "ping") {
        return handle_ping(request.id_json);
    }

    if (request.method == "tools/list") {
        return handle_tools_list(request.id_json);
    }

    if (request.method == "tools/call") {
        return handle_tools_call(
            request.id_json,
            request.params_json
        );
    }

    return jsonrpc::make_error(
        request.id_json,
        jsonrpc::METHOD_NOT_FOUND,
        "Method not found: " + request.method
    );
}

std::string RequestHandler::handle_initialize(
    const std::string& id_json,
    const std::string& params_json
)
{
    JsonPtr params = parse_json(params_json);

    if (!params ||
        json_object_get_type(params.get()) != json_type_object) {
        return invalid_params(
            id_json,
            "initialize requires an object params"
        );
    }

    std::string client_protocol;

    if (!get_string(
            params.get(),
            "protocolVersion",
            client_protocol)) {
        return invalid_params(
            id_json,
            "Missing or invalid protocolVersion"
        );
    }

    json_object* capabilities = nullptr;
    json_object* client_info = nullptr;

    if (!json_object_object_get_ex(
            params.get(),
            "capabilities",
            &capabilities) ||
        capabilities == nullptr ||
        json_object_get_type(capabilities) != json_type_object) {
        return invalid_params(
            id_json,
            "Missing or invalid client capabilities"
        );
    }

    if (!json_object_object_get_ex(
            params.get(),
            "clientInfo",
            &client_info) ||
        client_info == nullptr ||
        json_object_get_type(client_info) != json_type_object) {
        return invalid_params(
            id_json,
            "Missing or invalid clientInfo"
        );
    }

    // Esta implementación anuncia una única versión MCP.
    // Si el cliente propone otra, el servidor devuelve la
    // versión que soporta para que el cliente decida si continúa.
    JsonPtr result(json_object_new_object(), json_object_put);
    JsonPtr server_info(json_object_new_object(), json_object_put);
    JsonPtr server_capabilities(json_object_new_object(), json_object_put);
    JsonPtr tools_capability(json_object_new_object(), json_object_put);

    if (!result || !server_info ||
        !server_capabilities || !tools_capability) {
        return jsonrpc::make_error(
            id_json,
            jsonrpc::INTERNAL_ERROR,
            "Cannot allocate initialize response"
        );
    }

    if (!add_string(
            result.get(),
            "protocolVersion",
            MCP_PROTOCOL_VERSION) ||
        !add_string(
            server_info.get(),
            "name",
            SERVER_NAME) ||
        !add_string(
            server_info.get(),
            "version",
            SERVER_VERSION)) {
        return jsonrpc::make_error(
            id_json,
            jsonrpc::INTERNAL_ERROR,
            "Cannot build initialize response"
        );
    }

    if (!add_object(
            server_capabilities.get(),
            "tools",
            tools_capability)) {
        return jsonrpc::make_error(
            id_json,
            jsonrpc::INTERNAL_ERROR,
            "Cannot build tool capabilities"
        );
    }

    if (!add_object(
            result.get(),
            "capabilities",
            server_capabilities) ||
        !add_object(
            result.get(),
            "serverInfo",
            server_info)) {
        return jsonrpc::make_error(
            id_json,
            jsonrpc::INTERNAL_ERROR,
            "Cannot build initialize response"
        );
    }

    std::string response = jsonrpc::make_result(
        id_json,
        serialize_json(result.get())
    );

    if (response.empty()) {
        return jsonrpc::make_error(
            id_json,
            jsonrpc::INTERNAL_ERROR,
            "Cannot serialize initialize response"
        );
    }

    initialize_response_sent_ = true;

    return response;
}

std::string RequestHandler::handle_ping(
    const std::string& id_json
)
{
    return jsonrpc::make_result(id_json, "{}");
}

std::string RequestHandler::handle_tools_list(
    const std::string& id_json
)
{
    JsonPtr result(json_object_new_object(), json_object_put);
    JsonPtr tools(json_object_new_array(), json_object_put);

    if (!result || !tools) {
        return jsonrpc::make_error(
            id_json,
            jsonrpc::INTERNAL_ERROR,
            "Cannot allocate tools/list response"
        );
    }

    const auto registered_tools = plugin_manager_.tools_snapshot();
    for (const RegisteredTool& registered : registered_tools) {
        std::string denial_reason;

        if (!authorization_policy_.authorize(
            peer_identity_,
            registered.name,
            registered.required_capabilities,
            denial_reason)) {
            continue;
        }

        JsonPtr tool(json_object_new_object(), json_object_put);
        JsonPtr input_schema = parse_json(
            registered.input_schema_json
        );

        if (!tool || !input_schema ||
            json_object_get_type(input_schema.get()) != json_type_object) {
            return jsonrpc::make_error(
                id_json,
                jsonrpc::INTERNAL_ERROR,
                "Plugin supplied an invalid input schema: " +
                    registered.name
            );
        }

        if (!add_string(
                tool.get(),
                "name",
                registered.name) ||
            !add_string(
                tool.get(),
                "description",
                registered.description) ||
            !add_object(
                tool.get(),
                "inputSchema",
                input_schema)) {
            return jsonrpc::make_error(
                id_json,
                jsonrpc::INTERNAL_ERROR,
                "Cannot construct tool definition"
            );
        }

        if (json_object_array_add(
                tools.get(),
                tool.release()) != 0) {
            return jsonrpc::make_error(
                id_json,
                jsonrpc::INTERNAL_ERROR,
                "Cannot append tool definition"
            );
        }
    }

    if (!add_array(result.get(), "tools", tools)) {
        return jsonrpc::make_error(
            id_json,
            jsonrpc::INTERNAL_ERROR,
            "Cannot construct tools/list result"
        );
    }

    return jsonrpc::make_result(
        id_json,
        serialize_json(result.get())
    );
}

std::string RequestHandler::make_tool_error(
    const std::string& message
) const
{
    JsonPtr result(json_object_new_object(), json_object_put);
    JsonPtr content(json_object_new_array(), json_object_put);
    JsonPtr text_content(json_object_new_object(), json_object_put);

    if (!result || !content || !text_content) {
        return "{}";
    }

    if (!add_string(text_content.get(), "type", "text") ||
        !add_string(text_content.get(), "text", message)) {
        return "{}";
    }

    if (json_object_array_add(
            content.get(),
            text_content.release()) != 0) {
        return "{}";
    }

    json_object* error_value = json_object_new_boolean(1);

    if (error_value == nullptr ||
        json_object_object_add(
            result.get(),
            "isError",
            error_value) != 0) {
        json_object_put(error_value);
        return "{}";
    }

    if (!add_array(result.get(), "content", content)) {
        return "{}";
    }

    return serialize_json(result.get());
}

std::string RequestHandler::handle_tools_call(
    const std::string& id_json,
    const std::string& params_json
)
{
    JsonPtr params = parse_json(params_json);

    if (!params ||
        json_object_get_type(params.get()) != json_type_object) {
        return invalid_params(
            id_json,
            "tools/call requires object params"
        );
    }

    std::string tool_name;

    if (!get_string(params.get(), "name", tool_name)) {
        return invalid_params(
            id_json,
            "tools/call requires a non-empty tool name"
        );
    }

    std::string arguments_json = "{}";
    json_object* arguments = nullptr;

    if (json_object_object_get_ex(
            params.get(),
            "arguments",
            &arguments)) {
        if (arguments == nullptr ||
            json_object_get_type(arguments) != json_type_object) {
            return invalid_params(
                id_json,
                "Tool arguments must be a JSON object"
            );
        }

        arguments_json = serialize_json(arguments);
    }

    const RegisteredTool* registered_tool = nullptr;

    const auto registered_tools = plugin_manager_.tools_snapshot();
    for (const RegisteredTool& tool : registered_tools) {
        if (tool.name == tool_name) {
            registered_tool = &tool;
            break;
        }
    }

    if (registered_tool == nullptr) {
        return invalid_params(
            id_json,
            "Unknown tool: " + tool_name
        );
    }

    std::string denial_reason;

    if (!authorization_policy_.authorize(
        peer_identity_,
        registered_tool->name,
        registered_tool->required_capabilities,
        denial_reason)) {
        return jsonrpc::make_result(
            id_json,
            make_tool_error("Permission denied")
        );
        }


    std::string tool_result;
    std::string execution_error;

    if (!plugin_manager_.execute_tool(
        tool_name,
        arguments_json,
        tool_result,
        execution_error)) {
        return jsonrpc::make_result(
            id_json,
            make_tool_error(execution_error)
        );
    }

    JsonPtr result(parse_json(tool_result));

    if (!result) {
        return jsonrpc::make_result(
            id_json,
            make_tool_error(
                "Plugin returned invalid JSON: " + tool_name
            )
        );
    }

    // MCP tools return content blocks. Plugin output is
    // represented as text, preserving its JSON serialization.
    JsonPtr response(json_object_new_object(), json_object_put);
    JsonPtr content(json_object_new_array(), json_object_put);
    JsonPtr text_content(json_object_new_object(), json_object_put);

    if (!response || !content || !text_content) {
        return jsonrpc::make_error(
            id_json,
            jsonrpc::INTERNAL_ERROR,
            "Cannot allocate tool result"
        );
    }

    if (!add_string(text_content.get(), "type", "text") ||
        !add_string(
            text_content.get(),
            "text",
            serialize_json(result.get()))) {
        return jsonrpc::make_error(
            id_json,
            jsonrpc::INTERNAL_ERROR,
            "Cannot construct tool content"
        );
    }

    if (json_object_array_add(
            content.get(),
            text_content.release()) != 0) {
        return jsonrpc::make_error(
            id_json,
            jsonrpc::INTERNAL_ERROR,
            "Cannot append tool content"
        );
    }

    if (!add_array(response.get(), "content", content)) {
        return jsonrpc::make_error(
            id_json,
            jsonrpc::INTERNAL_ERROR,
            "Cannot construct tool response"
        );
    }

    json_object* is_error = json_object_new_boolean(0);

    if (is_error == nullptr ||
        json_object_object_add(
            response.get(),
            "isError",
            is_error) != 0) {
        json_object_put(is_error);
        return jsonrpc::make_error(
            id_json,
            jsonrpc::INTERNAL_ERROR,
            "Cannot construct tool response"
        );
    }

    return jsonrpc::make_result(
        id_json,
        serialize_json(response.get())
    );
}

} // namespace mcp
