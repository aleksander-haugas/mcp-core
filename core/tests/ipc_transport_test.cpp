#include "mcp/ipc_transport.hpp"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
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

void alarm_handler(int)
{
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
            return true;
        }

        line.push_back(character);

        if (line.size() > 16u * 1024u * 1024u) {
            return false;
        }
    }
}

int connect_client(const std::string& path)
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

    if (connect(
            fd,
            reinterpret_cast<struct sockaddr*>(&address),
            sizeof(address)
        ) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

} // namespace

int main()
{
    char directory_template[] = "/tmp/mcp-ipc-test-XXXXXX";

    char* directory = mkdtemp(directory_template);

    check(directory != nullptr, "Temporary directory created");

    if (directory == nullptr) {
        return EXIT_FAILURE;
    }

    const std::string socket_path =
        std::string(directory) + "/core.sock";

    const pid_t child = fork();

    check(child >= 0, "Server process forked");

    if (child < 0) {
        rmdir(directory);
        return EXIT_FAILURE;
    }

    if (child == 0) {
        struct sigaction action {};
        action.sa_handler = alarm_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;

        sigaction(SIGALRM, &action, nullptr);

        alarm(5);

        mcp::PluginManager plugin_manager;

        if (!plugin_manager.load_directory(
                "core/tests/plugins",
                "{}")) {
            _exit(2);
        }

        mcp::IpcServer server(plugin_manager, socket_path);

        const int result = server.run();

        plugin_manager.shutdown();

        _exit(result == 0 ? 0 : 3);
    }

    int client_fd = -1;

    for (int attempt = 0; attempt < 300; ++attempt) {
        client_fd = connect_client(socket_path);

        if (client_fd >= 0) {
            break;
        }

        usleep(10000);
    }

    check(client_fd >= 0, "Client connects to Unix socket");

    if (client_fd < 0) {
        kill(child, SIGTERM);
        waitpid(child, nullptr, 0);
        unlink(socket_path.c_str());
        rmdir(directory);
        return EXIT_FAILURE;
    }

    struct stat socket_info {};

    check(
        lstat(socket_path.c_str(), &socket_info) == 0 &&
        S_ISSOCK(socket_info.st_mode),
        "IPC endpoint is a Unix socket"
    );

    check(
        (socket_info.st_mode & 0777) == 0660,
          "Socket permissions are 0660"
    );

    const std::string initialize =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"ipc-test","version":"1.0"}}})";

    const std::string initialized =
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})";

    const std::string ping =
        R"({"jsonrpc":"2.0","id":2,"method":"ping"})";

    check(
        send_all(
            client_fd,
            initialize + "\n" + initialized + "\n" + ping + "\n"
        ),
        "Client sends initialize, notification and ping"
    );

    std::string response;

    const bool received_initialize =
        receive_line(client_fd, response) &&
        response.find(R"("id":1)") != std::string::npos &&
        response.find(R"("protocolVersion")") != std::string::npos;

    check(
        received_initialize,
        "Server returns initialize response"
    );

    const bool received_ping =
        receive_line(client_fd, response) &&
        response.find(R"("id":2)") != std::string::npos &&
        response.find(R"("result")") != std::string::npos;

    check(
        received_ping,
        "Server returns ping response"
    );

    close(client_fd);

    // A second client must receive an independent MCP session.
    client_fd = connect_client(socket_path);

    check(
        client_fd >= 0,
        "Second client connects"
    );

    if (client_fd >= 0) {
        check(
            send_all(
                client_fd,
                R"({"jsonrpc":"2.0","id":3,"method":"ping"})" "\n"
            ),
            "Second client sends ping"
        );

        const bool independent_session =
            receive_line(client_fd, response) &&
            response.find(R"("code":-32600)") != std::string::npos;

        check(
            independent_session,
            "Second client starts with an independent MCP session"
        );

        close(client_fd);
    }

    int child_status = 0;
    const pid_t waited = waitpid(child, &child_status, 0);

    check(
        waited == child && WIFEXITED(child_status) &&
        WEXITSTATUS(child_status) == 0,
        "IPC server exits cleanly"
    );

    check(
        lstat(socket_path.c_str(), &socket_info) < 0 &&
        errno == ENOENT,
        "Socket is removed after server shutdown"
    );

    rmdir(directory);

    std::cout << "\nIPC TRANSPORT TESTS: "
              << (failures == 0 ? "ALL PASS" : "FAILURES")
              << " (" << checks << " checks, "
              << failures << " failures)\n";

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
