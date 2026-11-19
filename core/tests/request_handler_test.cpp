#include "mcp/request_handler.hpp"

#include <json-c/json.h>
#include "mcp/authorization.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string& name)
{
    ++checks;

    if (condition) {
        std::cout << "[PASS] " << name << '\n';
    } else {
        ++failures;
        std::cerr << "[FAIL] " << name << '\n';
    }
}

json_object* parse(const std::string& input)
{
    return json_tokener_parse(input.c_str());
}

json_object* member(json_object* object, const char* key)
{
    json_object* value = nullptr;

    if (object == nullptr ||
        !json_object_object_get_ex(object, key, &value)) {
        return nullptr;
    }

    return value;
}

bool has_error_code(const std::string& response, int expected)
{
    json_object* root = parse(response);
    if (root == nullptr) {
        return false;
    }

    json_object* error = member(root, "error");
    json_object* code = member(error, "code");

    const bool result =
        code != nullptr &&
        json_object_get_int(code) == expected;

    json_object_put(root);
    return result;
}

bool has_result(const std::string& response)
{
    json_object* root = parse(response);
    if (root == nullptr) {
        return false;
    }

    const bool result = member(root, "result") != nullptr;
    json_object_put(root);
    return result;
}

} // namespace

int main()
{
    mcp::PluginManager plugin_manager;

    check(
        plugin_manager.load_directory(
            "core/tests/plugins",
            "{}"
        ),
        "Test plugin loads"
    );

    check(
        plugin_manager.plugin_count() == 1,
        "One plugin registered"
    );

    check(
        plugin_manager.tool_count() == 1,
        "One tool registered"
    );

    mcp::AuthorizationPolicy test_policy({
        {
            static_cast<uid_t>(1001),
                                         {"test.echo"}
        }
    });

    const mcp::PeerIdentity test_identity{
        static_cast<uid_t>(1001),
        static_cast<gid_t>(1001),
        true
    };

    mcp::RequestHandler handler(
        plugin_manager,
        test_policy,
        test_identity
    );

    check(
        !handler.initialized(),
        "Handler starts uninitialized"
    );

    const std::string ping_before_initialize =
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})";

    const std::string ping_before_response =
        handler.handle(ping_before_initialize);

    check(
        has_error_code(ping_before_response, -32600),
        "Ping rejected before initialization"
    );

    const std::string initialize_request =
        R"({
            "jsonrpc":"2.0",
            "id":2,
            "method":"initialize",
            "params":{
                "protocolVersion":"2025-11-25",
                "capabilities":{},
                "clientInfo":{
                    "name":"mcp-core-test",
                    "version":"1.0"
                }
            }
        })";

    const std::string initialize_response =
        handler.handle(initialize_request);

    check(
        has_result(initialize_response),
        "Initialize returns a result"
    );

    check(
        !handler.initialized(),
        "Initialize response alone does not complete initialization"
    );

    json_object* init_root = parse(initialize_response);
    json_object* init_result = member(init_root, "result");

    check(
        member(init_result, "protocolVersion") != nullptr,
        "Initialize response contains protocolVersion"
    );

    check(
        member(init_result, "capabilities") != nullptr,
        "Initialize response contains capabilities"
    );

    check(
        member(init_result, "serverInfo") != nullptr,
        "Initialize response contains serverInfo"
    );

    if (init_root != nullptr) {
        json_object_put(init_root);
    }

    const std::string initialized_notification =
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})";

    const std::string notification_response =
        handler.handle(initialized_notification);

    check(
        notification_response.empty(),
        "Initialized notification produces no response"
    );

    check(
        handler.initialized(),
        "Initialized notification completes initialization"
    );

    const std::string ping_request =
        R"({"jsonrpc":"2.0","id":3,"method":"ping"})";

    check(
        has_result(handler.handle(ping_request)),
        "Ping succeeds after initialization"
    );

    const std::string tools_list_request =
        R"({"jsonrpc":"2.0","id":4,"method":"tools/list"})";

    const std::string tools_list_response =
        handler.handle(tools_list_request);

    json_object* list_root = parse(tools_list_response);
    json_object* list_result = member(list_root, "result");
    json_object* tools = member(list_result, "tools");

    bool tool_found = false;
    bool schema_found = false;

    if (tools != nullptr &&
        json_object_is_type(tools, json_type_array)) {
        const size_t count = json_object_array_length(tools);

        for (size_t i = 0; i < count; ++i) {
            json_object* tool = json_object_array_get_idx(tools, i);
            json_object* name = member(tool, "name");

            if (name != nullptr &&
                std::string(json_object_get_string(name)) ==
                    "core_test_echo") {
                tool_found = true;
                schema_found = member(tool, "inputSchema") != nullptr;
            }
        }
    }

    check(tool_found, "tools/list includes core_test_echo");
    check(schema_found, "Tool definition includes inputSchema");

    if (list_root != nullptr) {
        json_object_put(list_root);
    }

    const std::string tools_call_request =
        R"({
            "jsonrpc":"2.0",
            "id":5,
            "method":"tools/call",
            "params":{
                "name":"core_test_echo",
                "arguments":{}
            }
        })";

    const std::string tools_call_response =
        handler.handle(tools_call_request);

    json_object* call_root = parse(tools_call_response);
    json_object* call_result = member(call_root, "result");
    json_object* content = member(call_result, "content");

    bool content_valid = false;
    bool echo_found = false;
    bool is_error_false = false;

    if (content != nullptr &&
        json_object_is_type(content, json_type_array) &&
        json_object_array_length(content) > 0) {
        json_object* first =
            json_object_array_get_idx(content, 0);

        json_object* type = member(first, "type");
        json_object* text = member(first, "text");

        content_valid =
            type != nullptr &&
            std::string(json_object_get_string(type)) == "text" &&
            text != nullptr;

        if (text != nullptr) {
            echo_found =
                std::string(json_object_get_string(text))
                    .find("hello from test plugin") !=
                std::string::npos;
        }
    }

    json_object* is_error = member(call_result, "isError");

    is_error_false =
        is_error != nullptr &&
        !json_object_get_boolean(is_error);

    check(content_valid, "tools/call returns text content");
    check(echo_found, "Plugin echo result reaches MCP response");
    check(is_error_false, "Successful tool call has isError=false");

    if (call_root != nullptr) {
        json_object_put(call_root);
    }

    const std::string unknown_tool_request =
        R"({
            "jsonrpc":"2.0",
            "id":6,
            "method":"tools/call",
            "params":{
                "name":"does_not_exist",
                "arguments":{}
            }
        })";

    check(
        has_error_code(handler.handle(unknown_tool_request), -32602),
        "Unknown tool is rejected"
    );

    check(
        handler.handle(
            R"({"jsonrpc":"2.0","method":"notifications/ping"})"
        ).empty(),
        "Notification produces no JSON-RPC response"
    );

    check(
        has_error_code(
            handler.handle(R"({"jsonrpc":"2.0","id":7,"method":})"),
            -32700
        ),
        "Malformed JSON returns parse error"
    );

    check(
        has_error_code(
            handler.handle(R"({"jsonrpc":"1.0","id":8,"method":"ping"})"),
            -32600
        ),
        "Invalid JSON-RPC version is rejected"
    );

    check(
        has_error_code(
            handler.handle(initialize_request),
            -32600
        ),
        "Duplicate initialize is rejected"
    );

    handler.reset();

    check(
        !handler.initialized(),
        "Reset clears initialized state"
    );

    check(
        has_error_code(handler.handle(ping_request), -32600),
        "Ping rejected after reset"
    );

    plugin_manager.shutdown();

    std::cout << "\nREQUEST HANDLER TESTS: "
              << (failures == 0 ? "ALL PASS" : "FAILURES")
              << " (" << checks << " checks, "
              << failures << " failures)\n";

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
