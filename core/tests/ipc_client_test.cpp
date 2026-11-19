#include "mcp/ipc_client.hpp"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
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
    char directory_template[] = "/tmp/mcp-ipc-client-XXXXXX";

    char* directory = mkdtemp(directory_template);

    check(directory != nullptr, "Temporary directory created");

    if (directory == nullptr) {
        return EXIT_FAILURE;
    }

    const std::string socket_path =
        std::string(directory) + "/client.sock";

    const pid_t child = fork();

    check(child >= 0, "Test server process forked");

    if (child < 0) {
        rmdir(directory);
        return EXIT_FAILURE;
    }

    if (child == 0) {
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

        std::string first_request;
        std::string second_request;

        const bool received_first =
            receive_line(client, first_request);

        const bool received_second =
            receive_line(client, second_request);

        const std::string responses =
            R"({"jsonrpc":"2.0","id":1,"result":{"message":"first"}})"
            "\n"
            R"({"jsonrpc":"2.0","id":2,"result":{"message":"second"}})"
            "\r\n";

        const std::size_t split = responses.size() / 2;

        const bool sent_first =
            send_all(client, responses.substr(0, split));

        const bool sent_second =
            sent_first &&
            send_all(client, responses.substr(split));

        close(client);
        close(listener);
        unlink(socket_path.c_str());

        _exit(
            received_first &&
            received_second &&
            first_request == R"({"jsonrpc":"2.0","id":1,"method":"ping"})" &&
            second_request == R"({"jsonrpc":"2.0","id":2,"method":"ping"})" &&
            sent_second ? 0 : 4
        );
    }

    mcp::IpcClient client(socket_path);
    std::string error;

    bool connected = false;

    for (int attempt = 0; attempt < 300; ++attempt) {
        if (client.connect(error)) {
            connected = true;
            break;
        }

        usleep(10000);
    }

    check(connected, "Client connects to Unix socket");
    check(client.connected(), "Client reports connected state");

    if (!connected) {
        kill(child, SIGTERM);
        waitpid(child, nullptr, 0);
        unlink(socket_path.c_str());
        rmdir(directory);
        return EXIT_FAILURE;
    }

    check(
        client.send_message(
            R"({"jsonrpc":"2.0","id":1,"method":"ping"})",
            error
        ),
        "Client sends first request"
    );

    check(
        client.send_message(
            R"({"jsonrpc":"2.0","id":2,"method":"ping"})",
            error
        ),
        "Client sends second request"
    );

    check(
        !client.send_message("invalid\nembedded", error),
        "Client rejects embedded newline"
    );

    std::string response;

    const bool first_response =
        client.receive_response(response, error) &&
        response.find(R"("id":1)") != std::string::npos &&
        response.find("first") != std::string::npos;

    check(
        first_response,
        "Client receives first response"
    );

    const bool second_response =
        client.receive_response(response, error) &&
        response.find(R"("id":2)") != std::string::npos &&
        response.find("second") != std::string::npos;

    check(
        second_response,
        "Client preserves buffered second response and accepts CRLF"
    );

    client.disconnect();

    check(
        !client.connected(),
        "Client disconnect resets connection state"
    );

    check(
        !client.send_message("{}", error),
        "Client rejects send after disconnect"
    );

    int child_status = 0;

    const pid_t waited = waitpid(child, &child_status, 0);

    check(
        waited == child &&
        WIFEXITED(child_status) &&
        WEXITSTATUS(child_status) == 0,
        "Test server exits successfully"
    );

    unlink(socket_path.c_str());
    rmdir(directory);

    std::cout << "\nIPC CLIENT TESTS: "
              << (failures == 0 ? "ALL PASS" : "FAILURES")
              << " (" << checks << " checks, "
              << failures << " failures)\n";

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
