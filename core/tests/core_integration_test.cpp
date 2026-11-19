
#include "mcp/ipc_transport.hpp"
#include "mcp/ipc_client.hpp"
#include "mcp/plugin_manager.hpp"
#include "mcp/stdio_bridge.hpp"
#include "mcp/authorization.hpp"

#include <json-c/json.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <signal.h>
#include <unistd.h>
#include <utility>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

    int checks = 0;
    int failures = 0;

    volatile sig_atomic_t stop_requested = 0;

    void handle_signal(int signal_number)
    {
        if (signal_number == SIGTERM || signal_number == SIGINT) {
            stop_requested = 1;
        }
    }

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

    bool response_has_id(
        const std::string& response,
        int expected_id
    )
    {
        json_object* root = parse(response);

        if (root == nullptr) {
            return false;
        }

        json_object* id = member(root, "id");

        const bool valid =
        id != nullptr &&
        json_object_get_int(id) == expected_id;

        json_object_put(root);
        return valid;
    }

    bool response_has_result(const std::string& response)
    {
        json_object* root = parse(response);
        if (root == nullptr) {
            return false;
        }

        const bool valid = member(root, "result") != nullptr;

        json_object_put(root);
        return valid;
    }

    bool initialize_valid(const std::string& response)
    {
        json_object* root = parse(response);
        if (root == nullptr) {
            return false;
        }

        json_object* result = member(root, "result");
        json_object* protocol = member(result, "protocolVersion");
        json_object* server = member(result, "serverInfo");

        const bool valid =
        protocol != nullptr &&
        std::string(json_object_get_string(protocol)) ==
        "2025-11-25" &&
        server != nullptr;

        json_object_put(root);
        return valid;
    }

    bool tools_list_valid(const std::string& response)
    {
        json_object* root = parse(response);
        if (root == nullptr) {
            return false;
        }

        json_object* result = member(root, "result");
        json_object* tools = member(result, "tools");

        bool found = false;

        if (tools != nullptr &&
            json_object_is_type(tools, json_type_array)) {

            const size_t count = json_object_array_length(tools);

        for (size_t i = 0; i < count; ++i) {
            json_object* tool =
            json_object_array_get_idx(tools, i);

            json_object* name = member(tool, "name");

            if (name != nullptr &&
                std::string(json_object_get_string(name)) ==
                "core_test_echo") {
                found = member(tool, "inputSchema") != nullptr;
                }
        }
            }

            json_object_put(root);
            return found;
    }

    bool tools_list_excludes_test_plugin(
        const std::string& response
    )
    {
        json_object* root = parse(response);

        if (root == nullptr) {
            return false;
        }

        json_object* result = member(root, "result");
        json_object* tools = member(result, "tools");

        bool valid =
        tools != nullptr &&
        json_object_is_type(tools, json_type_array);

        if (valid) {
            const size_t count = json_object_array_length(tools);

            for (size_t i = 0; i < count; ++i) {
                json_object* tool =
                json_object_array_get_idx(tools, i);

                json_object* name = member(tool, "name");

                if (name != nullptr &&
                    std::string(json_object_get_string(name)) ==
                    "core_test_echo") {
                    valid = false;
                break;
                    }
            }
        }

        json_object_put(root);
        return valid;
    }

    bool tool_call_valid(const std::string& response)
    {
        json_object* root = parse(response);
        if (root == nullptr) {
            return false;
        }

        json_object* result = member(root, "result");
        json_object* content = member(result, "content");

        bool found = false;

        if (content != nullptr &&
            json_object_is_type(content, json_type_array) &&
            json_object_array_length(content) > 0) {

            json_object* first =
            json_object_array_get_idx(content, 0);

        json_object* type = member(first, "type");
        json_object* text = member(first, "text");

        if (type != nullptr && text != nullptr) {
            found =
            std::string(json_object_get_string(type)) == "text" &&
            std::string(json_object_get_string(text))
            .find("hello from test plugin") !=
            std::string::npos;
        }
            }

            json_object_put(root);
            return found;
    }

    bool tool_call_denied(const std::string& response)
    {
        json_object* root = parse(response);

        if (root == nullptr) {
            return false;
        }

        json_object* result = member(root, "result");
        json_object* is_error = member(result, "isError");

        const bool denied =
        is_error != nullptr &&
        json_object_get_boolean(is_error);

        json_object_put(root);
        return denied;
    }

    bool wait_for_socket(
        const std::string& path,
        int timeout_ms
    )
    {
        const int interval_ms = 20;

        for (int elapsed = 0; elapsed < timeout_ms;
             elapsed += interval_ms) {

            struct stat info {};

        if (lstat(path.c_str(), &info) == 0 &&
            S_ISSOCK(info.st_mode)) {
            return true;
            }

            usleep(interval_ms * 1000);
             }

             return false;
    }

    bool wait_for_child(pid_t child, int& status)
    {
        for (int elapsed = 0; elapsed < 3000; elapsed += 20) {
            const pid_t result = waitpid(child, &status, WNOHANG);

            if (result == child) {
                return true;
            }

            if (result < 0 && errno != EINTR) {
                return false;
            }

            usleep(20000);
        }

        kill(child, SIGKILL);

        while (waitpid(child, &status, 0) < 0) {
            if (errno != EINTR) {
                return false;
            }
        }

        return false;
    }

} // namespace

int main()
{
    char directory_template[] = "/tmp/mcp-core-integration-XXXXXX";
    char* directory = mkdtemp(directory_template);

    check(directory != nullptr, "Temporary directory created");

    if (directory == nullptr) {
        return EXIT_FAILURE;
    }

    const std::string socket_path =
    std::string(directory) + "/mcp.sock";

    // Evitar que los mensajes previos al fork se dupliquen.
    std::cout.flush();
    std::cerr.flush();

    const pid_t child = fork();

    check(child >= 0, "Server process forked");

    if (child < 0) {
        rmdir(directory);
        return EXIT_FAILURE;
    }

    if (child == 0) {
        struct sigaction action {};
        action.sa_handler = handle_signal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;

        if (sigaction(SIGTERM, &action, nullptr) != 0 ||
            sigaction(SIGINT, &action, nullptr) != 0) {
            _exit(10);
            }

            // Límite de seguridad por si la parada controlada falla.
            alarm(10);

        mcp::PluginManager manager;

        if (!manager.load_directory("core/tests/plugins", "{}")) {
            _exit(11);
        }

        if (manager.plugin_count() != 1 ||
            manager.tool_count() != 1) {
            manager.shutdown();
        _exit(12);
            }

            mcp::AuthorizationPolicy integration_policy({
                {
                    geteuid(),
                                                        {"test.echo"}
                }
            });

            mcp::IpcServer server(
                manager,
                socket_path,
                std::move(integration_policy)
            );

            const int result = server.run(&stop_requested);

            manager.shutdown();

            _exit(result == 0 ? 0 : 13);
    }

    const bool socket_ready = wait_for_socket(socket_path, 3000);

    check(socket_ready, "Real IPC server creates Unix socket");

    if (!socket_ready) {
        kill(child, SIGTERM);

        int status = 0;
        wait_for_child(child, status);

        rmdir(directory);
        return EXIT_FAILURE;
    }

    struct stat socket_info {};

    const bool socket_permissions =
    lstat(socket_path.c_str(), &socket_info) == 0 &&
    (socket_info.st_mode & 0777) == 0660;

    check(
        socket_permissions,
        "IPC socket permissions are 0660"
    );

    mcp::IpcClient client(socket_path);
    std::string connection_error;

    const bool connected = client.connect(connection_error);

    check(connected, "Bridge client connects to real IPC server");

    if (!connected) {
        std::cerr << "[ERROR] " << connection_error << '\n';

        kill(child, SIGTERM);

        int status = 0;
        wait_for_child(child, status);

        rmdir(directory);
        return EXIT_FAILURE;
    }

    const std::string initialize =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"core-integration-test","version":"1.0"}}})";

    const std::string initialized =
    R"({"jsonrpc":"2.0","method":"notifications/initialized"})";

    const std::string ping =
    R"({"jsonrpc":"2.0","id":2,"method":"ping"})";

    const std::string tools_list =
    R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})";

    const std::string tools_call =
    R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"core_test_echo","arguments":{}}})";

    std::istringstream input(
        initialize + "\n" +
        initialized + "\n" +
        ping + "\n" +
        tools_list + "\n" +
        tools_call + "\n"
    );

    std::ostringstream output;
    std::ostringstream diagnostics;

    mcp::StdioBridge bridge(client);

    const int bridge_result =
    bridge.run(input, output, diagnostics);

    check(
        bridge_result == 0,
        "STDIO bridge completes real MCP session"
    );

    std::vector<std::string> responses;

    {
        std::istringstream response_stream(output.str());
        std::string line;

        while (std::getline(response_stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (!line.empty()) {
                responses.push_back(line);
            }
        }
    }

    check(
        responses.size() == 4,
          "Exactly four responses returned; notification omitted"
    );

    if (responses.size() == 4) {
        check(
            response_has_id(responses[0], 1) &&
            initialize_valid(responses[0]),
              "Real initialize response is valid"
        );

        check(
            response_has_id(responses[1], 2) &&
            response_has_result(responses[1]),
              "Real ping response is valid"
        );

        check(
            response_has_id(responses[2], 3) &&
            tools_list_valid(responses[2]),
              "Real tools/list exposes test plugin"
        );

        check(
            response_has_id(responses[3], 4) &&
            tool_call_valid(responses[3]),
              "Real tools/call executes plugin successfully"
        );
    }

    check(
        diagnostics.str().find("Input stream closed") !=
        std::string::npos,
        "Bridge reports STDIO closure"
    );

    // El cliente permanece conectado e inactivo durante SIGTERM.
    usleep(100000);

    const bool signal_sent = kill(child, SIGTERM) == 0;

    check(
        signal_sent,
        "SIGTERM sent while IPC client remains connected"
    );

    int child_status = 0;
    const bool child_stopped = wait_for_child(child, child_status);

    check(
        child_stopped &&
        WIFEXITED(child_status) &&
        WEXITSTATUS(child_status) == 0,
          "IPC server shuts down with an idle connected client"
    );

    // El cliente se desconecta después de terminar el servidor.
    client.disconnect();

    check(
        !client.connected(),
          "IPC client disconnects cleanly"
    );

    struct stat remaining_socket {};

    check(
        lstat(socket_path.c_str(), &remaining_socket) < 0 &&
        errno == ENOENT,
        "IPC server removes socket on shutdown"
    );

    // Segunda fase: servidor con política vacía.
    const std::string denied_socket_path =
    std::string(directory) + "/denied.sock";

    std::cout.flush();
    std::cerr.flush();

    const pid_t denied_child = fork();

    check(
        denied_child >= 0,
        "Denied-policy server process forked"
    );

    if (denied_child == 0) {
        stop_requested = 0;

        struct sigaction action {};
        action.sa_handler = handle_signal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;

        if (sigaction(SIGTERM, &action, nullptr) != 0 ||
            sigaction(SIGINT, &action, nullptr) != 0) {
            _exit(20);
            }

            mcp::PluginManager manager;

        if (!manager.load_directory("core/tests/plugins", "{}")) {
            _exit(21);
        }

        mcp::AuthorizationPolicy empty_policy(
            std::vector<mcp::CapabilityGrant>{}
        );

        mcp::IpcServer server(
            manager,
            denied_socket_path,
            std::move(empty_policy)
        );

        const int result = server.run(&stop_requested);

        manager.shutdown();

        _exit(result == 0 ? 0 : 22);
    }

    if (denied_child > 0) {
        const bool denied_socket_ready =
        wait_for_socket(denied_socket_path, 3000);

        check(
            denied_socket_ready,
            "Denied-policy IPC server creates socket"
        );

        if (denied_socket_ready) {
            mcp::IpcClient denied_client(denied_socket_path);
            std::string denied_error;

            const bool denied_connected =
            denied_client.connect(denied_error);

            check(
                denied_connected,
                "Unauthorised test client connects"
            );

            if (denied_connected) {
                const std::string denied_initialize =
                R"({"jsonrpc":"2.0","id":10,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"denied-test","version":"1.0"}}})";

                const std::string denied_initialized =
                R"({"jsonrpc":"2.0","method":"notifications/initialized"})";

                const std::string denied_list =
                R"({"jsonrpc":"2.0","id":11,"method":"tools/list"})";

                const std::string denied_call =
                R"({"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"core_test_echo","arguments":{}}})";

                std::string response;
                bool initialize_ok =
                denied_client.send_message(
                    denied_initialize,
                    denied_error
                ) &&
                denied_client.receive_response(
                    response,
                    denied_error
                ) &&
                response_has_id(response, 10) &&
                initialize_valid(response);

                check(
                    initialize_ok,
                    "Unauthorised client completes MCP initialization"
                );

                bool notification_ok =
                denied_client.send_message(
                    denied_initialized,
                    denied_error
                );

                bool list_ok =
                notification_ok &&
                denied_client.send_message(
                    denied_list,
                    denied_error
                ) &&
                denied_client.receive_response(
                    response,
                    denied_error
                ) &&
                response_has_id(response, 11) &&
                tools_list_excludes_test_plugin(response);

                check(
                    list_ok,
                    "tools/list hides protected test tool"
                );

                bool call_ok =
                denied_client.send_message(
                    denied_call,
                    denied_error
                ) &&
                denied_client.receive_response(
                    response,
                    denied_error
                ) &&
                response_has_id(response, 12) &&
                tool_call_denied(response);

                check(
                    call_ok,
                    "tools/call denies protected test tool"
                );

                denied_client.disconnect();

                check(
                    !denied_client.connected(),
                      "Unauthorised test client disconnects cleanly"
                );
            } else {
                std::cerr << "[ERROR] "
                << denied_error << '\n';
            }
        }

        kill(denied_child, SIGTERM);

        int denied_child_status = 0;
        const bool denied_child_stopped =
        wait_for_child(denied_child, denied_child_status);

        check(
            denied_child_stopped &&
            WIFEXITED(denied_child_status) &&
            WEXITSTATUS(denied_child_status) == 0,
              "Denied-policy IPC server shuts down cleanly"
        );

        struct stat denied_socket_info {};

        check(
            lstat(
                denied_socket_path.c_str(),
                  &denied_socket_info
            ) < 0 &&
            errno == ENOENT,
            "Denied-policy server removes socket"
        );
    }

    rmdir(directory);

    std::cout << "\nCORE INTEGRATION TESTS: "
    << (failures == 0 ? "ALL PASS" : "FAILURES")
    << " (" << checks << " checks, "
    << failures << " failures)\n";

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
