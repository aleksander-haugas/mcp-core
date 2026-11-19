
#include "mcp/json_rpc.hpp"

#include <json-c/json.h>

#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& name)
{
    if (condition) {
        std::cout << "[PASS] " << name << '\n';
    } else {
        std::cerr << "[FAIL] " << name << '\n';
        ++failures;
    }
}

json_object* parse_output(const std::string& json)
{
    if (json.empty()) {
        return nullptr;
    }

    return json_tokener_parse(json.c_str());
}

bool has_key(json_object* object, const char* key)
{
    json_object* value = nullptr;

    return object != nullptr &&
        json_object_object_get_ex(object, key, &value);
}

void test_valid_request()
{
    mcp::JsonRpcRequest request;
    std::string error;

    const bool parsed = mcp::jsonrpc::parse_request(
        R"({"jsonrpc":"2.0","method":"ping","id":1})",
        request,
        error
    );

    check(parsed, "Valid request parses");
    check(request.method == "ping", "Method extracted");
    check(request.has_id, "Request ID detected");
    check(request.id_json == "1", "Numeric ID preserved");
    check(!request.is_notification(), "Request is not notification");
}

void test_notification()
{
    mcp::JsonRpcRequest request;
    std::string error;

    const bool parsed = mcp::jsonrpc::parse_request(
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
        request,
        error
    );

    check(parsed, "Notification parses");
    check(!request.has_id, "Notification has no ID");
    check(request.is_notification(), "Notification detected");
}

void test_object_and_array_params()
{
    mcp::JsonRpcRequest object_request;
    mcp::JsonRpcRequest array_request;
    std::string error;

    const bool object_ok = mcp::jsonrpc::parse_request(
        R"({"jsonrpc":"2.0","method":"tools/list","params":{},"id":"a"})",
        object_request,
        error
    );

    check(object_ok, "Object params accepted");
    check(object_request.has_params, "Object params detected");

    const bool array_ok = mcp::jsonrpc::parse_request(
        R"({"jsonrpc":"2.0","method":"example","params":[1,2],"id":"b"})",
        array_request,
        error
    );

    check(array_ok, "Array params accepted");
    check(array_request.params_json == "[1,2]", "Array params preserved");
}

void test_invalid_requests()
{
    mcp::JsonRpcRequest request;
    std::string error;

    check(
        !mcp::jsonrpc::parse_request("{invalid", request, error),
        "Malformed JSON rejected"
    );

    check(
        !mcp::jsonrpc::parse_request(
            R"({"jsonrpc":"1.0","method":"ping","id":1})",
            request,
            error
        ),
        "Invalid JSON-RPC version rejected"
    );

    check(
        !mcp::jsonrpc::parse_request(
            R"({"jsonrpc":"2.0","id":1})",
            request,
            error
        ),
        "Missing method rejected"
    );

    check(
        !mcp::jsonrpc::parse_request(
            R"({"jsonrpc":"2.0","method":"ping","params":42,"id":1})",
            request,
            error
        ),
        "Scalar params rejected"
    );

    check(
        !mcp::jsonrpc::parse_request(
            R"({"jsonrpc":"2.0","method":"ping","id":true})",
            request,
            error
        ),
        "Boolean ID rejected"
    );

    check(
        !mcp::jsonrpc::parse_request(
            R"([{"jsonrpc":"2.0","method":"ping","id":1}])",
            request,
            error
        ),
        "Top-level array rejected"
    );
}

void test_success_response()
{
    const std::string response =
        mcp::jsonrpc::make_result(
            "7",
            R"({"status":"ok"})"
        );

    json_object* object = parse_output(response);

    check(object != nullptr, "Success response is valid JSON");
    check(has_key(object, "jsonrpc"), "Success has jsonrpc field");
    check(has_key(object, "id"), "Success has ID field");
    check(has_key(object, "result"), "Success has result field");
    check(!has_key(object, "error"), "Success has no error field");

    if (object != nullptr) {
        json_object_put(object);
    }
}

void test_error_response()
{
    const std::string response =
        mcp::jsonrpc::make_error(
            "null",
            mcp::jsonrpc::METHOD_NOT_FOUND,
            "Method not found",
            R"({"method":"unknown"})"
        );

    json_object* object = parse_output(response);
    json_object* error_object = nullptr;
    json_object* code = nullptr;

    const bool valid_error =
        object != nullptr &&
        json_object_object_get_ex(
            object,
            "error",
            &error_object
        ) &&
        error_object != nullptr &&
        json_object_object_get_ex(
            error_object,
            "code",
            &code
        ) &&
        code != nullptr &&
        json_object_get_int(code) == mcp::jsonrpc::METHOD_NOT_FOUND;

    check(object != nullptr, "Error response is valid JSON");
    check(has_key(object, "id"), "Error response has ID field");
    check(valid_error, "Error code preserved");
    check(!has_key(object, "result"), "Error has no result field");

    if (object != nullptr) {
        json_object_put(object);
    }
}

void test_invalid_response_inputs()
{
    check(
        mcp::jsonrpc::make_result("1", "{invalid").empty(),
        "Invalid result JSON rejected"
    );

    check(
        mcp::jsonrpc::make_error("not-json", -32600, "Invalid request").empty(),
        "Invalid error ID rejected"
    );

    check(
        mcp::jsonrpc::make_result("1", "null").size() > 0,
        "JSON null result accepted"
    );
}

} // namespace

int main()
{
    std::cout << "========== JSON-RPC 2.0 TESTS ==========\n";

    test_valid_request();
    test_notification();
    test_object_and_array_params();
    test_invalid_requests();
    test_success_response();
    test_error_response();
    test_invalid_response_inputs();

    std::cout << "\n========================================\n";

    if (failures != 0) {
        std::cerr << "JSON-RPC TESTS FAILED: "
                  << failures << '\n';
        return 1;
    }

    std::cout << "JSON-RPC TESTS: ALL PASS\n";
    return 0;
}
