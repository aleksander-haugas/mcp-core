
#include "mcp/ipc_client.hpp"
#include "mcp/stdio_bridge.hpp"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>

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

bool send_all(int fd, const std::string& data)
{
    std::size_t sent = 0;

    while (sent < data.size()) {
        const ssize_t result = send(
            fd,
            data.data() + sent,
            data.size() - sent,
            0
        );

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (result == 0) {
            return false;
        }

        sent += static_cast<std::size_t>(result);
    }

    return true;
}

bool receive_line(int fd, std::string& line)
{
    line.clear();

    while (true) {
        char character = '\0';

        const ssize_t result = recv(fd, &character, 1, 0);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (result == 0) {
            return false;
        }

        if (character == '\n') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return true;
        }

        line.push_back(character);

        if (line.size() > 16u * 1024u * 1024u) {
            return false;
        }
    }
}

int create_listener(const std::string& path)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;

    std::memcpy(
        address.sun_path,
        path.c_str(),
        path.size() + 1
    );

    if (bind(
            fd,
            reinterpret_cast<struct sockaddr*>(&address),
            sizeof(address)
        ) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        close(fd);
        unlink(path.c_str());
        return -1;
    }

    return fd;
}

} // namespace

int main()
{
    char directory_template[] = "/tmp/mcp-bridge-test-XXXXXX";

    char* directory = mkdtemp(directory_template);

    check(directory != nullptr, "Temporary directory created");

    if (directory == nullptr) {
        return EXIT_FAILURE;
    }

    const std::string socket_path =
        std::string(directory) + "/bridge.sock";

    const pid_t child = fork();

    check(child >= 0, "Mock IPC server process forked");

    if (child < 0) {
        rmdir(directory);
        return EXIT_FAILURE;
    }

    if (child == 0) {
        alarm(10);

        const int listener = create_listener(socket_path);

        if (listener < 0) {
            _exit(2);
        }

        const int client = accept(listener, nullptr, nullptr);

        if (client < 0) {
            close(listener);
            unlink(socket_path.c_str());
            _exit(3);
        }

        std::string initialize;
        std::string notification;
        std::string ping;

        // 1. Receive initialize.
        const bool received_initialize =
        receive_line(client, initialize);

        const bool initialize_valid =
        received_initialize &&
        initialize.find(R"("method":"initialize")") !=
        std::string::npos;

        const std::string initialize_response =
        R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-11-25"}})"
        "\n";

        const bool sent_initialize =
        initialize_valid &&
        send_all(client, initialize_response);

        // 2. Receive initialized notification.
        const bool received_notification =
        sent_initialize &&
        receive_line(client, notification);

        const bool notification_valid =
        received_notification &&
        notification.find(
            R"("method":"notifications/initialized")"
        ) != std::string::npos;

        // 3. Receive ping.
        const bool received_ping =
        notification_valid &&
        receive_line(client, ping);

        const bool ping_valid =
        received_ping &&
        ping.find(R"("method":"ping")") !=
        std::string::npos;

        const std::string ping_response =
        R"({"jsonrpc":"2.0","id":2,"result":{}})"
        "\r\n";

        // Deliberately split the response into two writes.
        const std::size_t split = ping_response.size() / 2;

        const bool sent_ping_first =
        ping_valid &&
        send_all(client, ping_response.substr(0, split));

        const bool sent_ping_second =
        sent_ping_first &&
        send_all(client, ping_response.substr(split));

        close(client);
        close(listener);
        unlink(socket_path.c_str());

        _exit(
            sent_ping_second ? 0 : 4
        );
    }

    mcp::IpcClient ipc_client(socket_path);

    std::string error;
    bool connected = false;

    for (int attempt = 0; attempt < 300; ++attempt) {
        if (ipc_client.connect(error)) {
            connected = true;
            break;
        }

        usleep(10000);
    }

    check(connected, "Bridge connects to IPC server");

    if (!connected) {
        kill(child, SIGTERM);
        waitpid(child, nullptr, 0);
        unlink(socket_path.c_str());
        rmdir(directory);
        return EXIT_FAILURE;
    }

    const std::string input_data =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"bridge-test","version":"1.0"}}})"
        "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
        "\n"
        R"({"jsonrpc":"2.0","id":2,"method":"ping"})"
        "\n";

    std::istringstream input(input_data);
    std::ostringstream output;
    std::ostringstream diagnostics;

    mcp::StdioBridge bridge(ipc_client);

    const int bridge_result =
        bridge.run(input, output, diagnostics);

    check(
        bridge_result == 0,
        "Bridge processes complete STDIO session"
    );

    const std::string expected_output =
        R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-11-25"}})"
        "\n"
        R"({"jsonrpc":"2.0","id":2,"result":{}})"
        "\n";

    check(
        output.str() == expected_output,
        "Bridge forwards responses to STDOUT and skips notification response"
    );

    check(
        diagnostics.str().find(
            "[MCP][BRIDGE] Input stream closed"
        ) != std::string::npos,
        "Bridge reports STDIO closure on diagnostics"
    );

    int child_status = 0;

    const pid_t waited = waitpid(child, &child_status, 0);

    check(
        waited == child &&
        WIFEXITED(child_status) &&
        WEXITSTATUS(child_status) == 0,
        "Mock server validates all three forwarded messages"
    );

    ipc_client.disconnect();

    check(
        !ipc_client.connected(),
        "IPC connection closes cleanly"
    );

    // An unconnected client must be rejected.
    mcp::IpcClient disconnected_client(socket_path);
    mcp::StdioBridge disconnected_bridge(disconnected_client);

    std::istringstream empty_input;
    std::ostringstream unused_output;
    std::ostringstream disconnected_diagnostics;

    check(
        disconnected_bridge.run(
            empty_input,
            unused_output,
            disconnected_diagnostics
        ) == 1,
        "Bridge rejects disconnected IPC client"
    );

    unlink(socket_path.c_str());
    rmdir(directory);

    std::cout << "\nSTDIO BRIDGE TESTS: "
              << (failures == 0 ? "ALL PASS" : "FAILURES")
              << " (" << checks << " checks, "
              << failures << " failures)\n";

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
