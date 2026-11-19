#include "mcp/stdio_transport.hpp"
#include "mcp/authorization.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
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

    bool contains(const std::string& value, const std::string& part)
    {
        return value.find(part) != std::string::npos;
    }

} // namespace

int main()
{
    mcp::PluginManager plugin_manager;

    check(
        plugin_manager.load_directory("core/tests/plugins", "{}"),
          "Test plugin loads"
    );

    // Esta prueba verifica el transporte STDIO, no los permisos
    // de ejecución de herramientas. La identidad queda sin autenticar.
    const mcp::AuthorizationPolicy test_policy(
        mcp::AuthorizationPolicy::default_local_policy()
    );

    const mcp::PeerIdentity test_identity{};

    mcp::RequestHandler handler(
        plugin_manager,
        test_policy,
        test_identity
    );

    mcp::StdioTransport transport(handler);

    const std::string initialize =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"stdio-test","version":"1.0"}}})";

    const std::string initialized =
    R"({"jsonrpc":"2.0","method":"notifications/initialized"})";

    const std::string ping =
    R"({"jsonrpc":"2.0","id":2,"method":"ping"})";

    // Empty line, CRLF initialization, notification, final message
    // without a trailing newline.
    std::istringstream input(
        "\n" +
        initialize + "\r\n" +
        initialized + "\r\n" +
        ping
    );

    std::ostringstream output;
    std::ostringstream diagnostics;

    const int result = transport.run(input, output, diagnostics);

    check(result == 0, "Transport exits successfully");
    check(handler.initialized(), "MCP initialization completed");

    const std::string output_text = output.str();

    std::istringstream response_lines(output_text);
    std::string line;
    int response_count = 0;

    bool parse_error_found = false;
    bool initialize_result_found = false;
    bool ping_result_found = false;

    while (std::getline(response_lines, line)) {
        if (line.empty()) {
            continue;
        }

        ++response_count;

        if (contains(line, R"("code":-32700)")) {
            parse_error_found = true;
        }

        if (contains(line, R"("protocolVersion")")) {
            initialize_result_found = true;
        }

        if (contains(line, R"("id":2)") &&
            contains(line, R"("result")")) {
            ping_result_found = true;
            }
    }

    check(
        response_count == 3,
        "Exactly three responses emitted"
    );

    check(
        parse_error_found,
        "Empty line produces JSON-RPC parse error"
    );

    check(
        initialize_result_found,
        "Initialize response emitted"
    );

    check(
        ping_result_found,
        "Final message without newline is processed"
    );

    check(
        contains(diagnostics.str(), "Input stream closed"),
          "EOF is reported on diagnostics"
    );

    check(
        !contains(output_text, "[MCP][STDIO]"),
          "Diagnostics do not contaminate stdout"
    );

    // Verify oversized messages are rejected.
    mcp::RequestHandler oversized_handler(
        plugin_manager,
        test_policy,
        test_identity
    );

    mcp::StdioTransport oversized_transport(oversized_handler);

    std::string oversized_message(
        16u * 1024u * 1024u + 1u,
        'A'
    );

    oversized_message.push_back('\n');

    std::istringstream oversized_input(oversized_message);
    std::ostringstream oversized_output;
    std::ostringstream oversized_diagnostics;

    const int oversized_result = oversized_transport.run(
        oversized_input,
        oversized_output,
        oversized_diagnostics
    );

    check(
        oversized_result == 1,
        "Oversized message is rejected"
    );

    check(
        oversized_output.str().empty(),
          "Oversized message produces no stdout response"
    );

    check(
        contains(oversized_diagnostics.str(), "exceeds"),
          "Oversized message error goes to stderr"
    );

    plugin_manager.shutdown();

    std::cout << "\nSTDIO TRANSPORT TESTS: "
    << (failures == 0 ? "ALL PASS" : "FAILURES")
    << " (" << checks << " checks, "
    << failures << " failures)\n";

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
