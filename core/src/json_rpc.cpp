
#include "mcp/json_rpc.hpp"

#include <json-c/json.h>

#include <cctype>
#include <climits>
#include <memory>
#include <string>

namespace mcp {
namespace jsonrpc {

namespace {

using JsonPtr = std::unique_ptr<json_object, decltype(&json_object_put)>;

JsonPtr parse_json(
    const std::string& input,
    std::string& error
)
{
    error.clear();

    if (input.empty() ||
        input.size() >= static_cast<std::size_t>(INT_MAX)) {
        error = "Empty or oversized JSON input";
    return JsonPtr(nullptr, json_object_put);
        }

        json_tokener* tokener = json_tokener_new();

        if (tokener == nullptr) {
            error = "Cannot allocate JSON tokener";
            return JsonPtr(nullptr, json_object_put);
        }

        json_tokener_set_flags(
            tokener,
            JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8
        );

        // Añadir un delimitador para que json-c finalice
        // correctamente el análisis de valores escalares.
        std::string parse_input = input;
        parse_input.push_back(' ');

        json_object* parsed = json_tokener_parse_ex(
            tokener,
            parse_input.data(),
                                                    static_cast<int>(parse_input.size())
        );

        const json_tokener_error parse_error =
        json_tokener_get_error(tokener);

        const std::size_t parse_end =
        json_tokener_get_parse_end(tokener);

        if (parse_error != json_tokener_success) {
            error = json_tokener_error_desc(parse_error);
            json_object_put(parsed);
            json_tokener_free(tokener);
            return JsonPtr(nullptr, json_object_put);
        }

        for (std::size_t i = parse_end; i < parse_input.size(); ++i) {
            if (!std::isspace(
                static_cast<unsigned char>(parse_input[i]))) {
                error = "Unexpected data after JSON value";
            json_object_put(parsed);
            json_tokener_free(tokener);
            return JsonPtr(nullptr, json_object_put);
                }
        }

        json_tokener_free(tokener);

        return JsonPtr(parsed, json_object_put);
}

std::string serialize_json(json_object* object)
{
    // En json-c, nullptr representa el valor JSON null.
    if (object == nullptr) {
        return "null";
    }

    const char* serialized = json_object_to_json_string_ext(
        object,
        JSON_C_TO_STRING_PLAIN
    );

    return serialized != nullptr
    ? std::string(serialized)
    : std::string();
}

bool valid_id_type(json_object* id)
{
    if (id == nullptr) {
        return true;
    }

    const json_type type = json_object_get_type(id);

    return type == json_type_string ||
           type == json_type_int ||
           type == json_type_double ||
           type == json_type_null;
}

bool add_string(
    json_object* object,
    const char* key,
    const std::string& value
)
{
    json_object* string_value = json_object_new_string_len(
        value.data(),
        static_cast<int>(value.size())
    );

    if (string_value == nullptr) {
        return false;
    }

    if (json_object_object_add(
            object,
            key,
            string_value) != 0) {
        json_object_put(string_value);
        return false;
    }

    return true;
}

} // namespace

bool parse_request(
    const std::string& input,
    JsonRpcRequest& request,
    std::string& error
)
{
    request = JsonRpcRequest{};
    error.clear();

    JsonPtr document = parse_json(input, error);

    if (!document) {
        error = "Invalid JSON: " + error;
        return false;
    }

    if (json_object_get_type(document.get()) != json_type_object) {
        error = "JSON-RPC message must be an object";
        return false;
    }

    json_object* version = nullptr;

    if (!json_object_object_get_ex(
            document.get(),
            "jsonrpc",
            &version) ||
        version == nullptr ||
        json_object_get_type(version) != json_type_string ||
        std::string(json_object_get_string(version)) != "2.0") {

        error = "Missing or invalid jsonrpc version";
        return false;
    }

    json_object* method = nullptr;

    if (!json_object_object_get_ex(
            document.get(),
            "method",
            &method) ||
        method == nullptr ||
        json_object_get_type(method) != json_type_string) {

        error = "Missing or invalid method";
        return false;
    }

    const char* method_value = json_object_get_string(method);
    const int method_length = json_object_get_string_len(method);

    if (method_value == nullptr || method_length <= 0) {
        error = "Method must not be empty";
        return false;
    }

    request.method.assign(
        method_value,
        static_cast<std::size_t>(method_length)
    );

    json_object* id = nullptr;

    if (json_object_object_get_ex(
            document.get(),
            "id",
            &id)) {

        if (!valid_id_type(id)) {
            error = "Invalid JSON-RPC id type";
            return false;
        }

        request.has_id = true;
        request.id_json = serialize_json(id);

        if (request.id_json.empty()) {
            error = "Cannot serialize JSON-RPC id";
            return false;
        }
    }

    json_object* params = nullptr;

    if (json_object_object_get_ex(
            document.get(),
            "params",
            &params)) {

        if (params == nullptr) {
            error = "Params must be an object or array";
            return false;
        }

        const json_type params_type = json_object_get_type(params);

        if (params_type != json_type_object &&
            params_type != json_type_array) {

            error = "Params must be an object or array";
            return false;
        }

        request.has_params = true;
        request.params_json = serialize_json(params);

        if (request.params_json.empty()) {
            error = "Cannot serialize JSON-RPC params";
            return false;
        }
    }

    return true;
}

std::string make_result(
    const std::string& id_json,
    const std::string& result_json
)
{
    std::string error;

    JsonPtr id = parse_json(id_json, error);

    if (!error.empty()) {
        return {};
    }

    JsonPtr result = parse_json(result_json, error);

    if (!result && !error.empty()) {
        return {};
    }

    JsonPtr response(json_object_new_object(), json_object_put);

    if (!response) {
        return {};
    }

    if (!add_string(response.get(), "jsonrpc", "2.0")) {
        return {};
    }

    json_object* id_value = id.release();

    if (json_object_object_add(
            response.get(),
            "id",
            id_value) != 0) {
        json_object_put(id_value);
        return {};
    }

    json_object* result_value = result.release();

    if (json_object_object_add(
            response.get(),
            "result",
            result_value) != 0) {
        json_object_put(result_value);
        return {};
    }

    return serialize_json(response.get());
}

std::string make_error(
    const std::string& id_json,
    int code,
    const std::string& message,
    const std::string& data_json
)
{
    std::string error;

    JsonPtr id = parse_json(id_json, error);

    if (!error.empty()) {
        return {};
    }

    JsonPtr response(json_object_new_object(), json_object_put);
    JsonPtr error_object(json_object_new_object(), json_object_put);

    if (!response || !error_object) {
        return {};
    }

    if (!add_string(response.get(), "jsonrpc", "2.0")) {
        return {};
    }

    json_object* id_value = id.release();

    if (json_object_object_add(
            response.get(),
            "id",
            id_value) != 0) {
        json_object_put(id_value);
        return {};
    }

    json_object* code_value = json_object_new_int(code);

    if (code_value == nullptr ||
        json_object_object_add(
            error_object.get(),
            "code",
            code_value) != 0) {
        json_object_put(code_value);
        return {};
    }

    if (!add_string(error_object.get(), "message", message)) {
        return {};
    }

    if (!data_json.empty()) {
        JsonPtr data = parse_json(data_json, error);

        if (!data && !error.empty()) {
            return {};
        }

        json_object* data_value = data.release();

        if (json_object_object_add(
                error_object.get(),
                "data",
                data_value) != 0) {
            json_object_put(data_value);
            return {};
        }
    }

    json_object* error_value = error_object.release();

    if (json_object_object_add(
            response.get(),
            "error",
            error_value) != 0) {
        json_object_put(error_value);
        return {};
    }

    return serialize_json(response.get());
}

} // namespace jsonrpc
} // namespace mcp
