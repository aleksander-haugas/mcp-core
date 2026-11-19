
#include "mcp/ipc_client.hpp"
#include "mcp/ipc_transport.hpp"
#include "mcp/plugin_manager.hpp"
#include "mcp/stdio_bridge.hpp"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

    volatile std::sig_atomic_t shutdown_requested = 0;

    constexpr const char* VERSION = "0.1.0";
    constexpr const char* DEFAULT_PLUGIN_DIR =
    "/usr/local/lib/mcp/plugins";
    constexpr const char* DEFAULT_SOCKET =
    "/var/run/mcp/mcp.sock";

    enum class RunMode {
        NONE,
        DAEMON,
        STDIO
    };

    void handle_signal(int signal_number)
    {
        if (signal_number == SIGINT || signal_number == SIGTERM) {
            shutdown_requested = 1;
        }
    }

    bool install_signal_handlers()
    {
        struct sigaction action {};

        action.sa_handler = handle_signal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;

        if (sigaction(SIGINT, &action, nullptr) != 0) {
            std::cerr << "[MCP][ERROR] Cannot install SIGINT handler\n";
            return false;
        }

        if (sigaction(SIGTERM, &action, nullptr) != 0) {
            std::cerr << "[MCP][ERROR] Cannot install SIGTERM handler\n";
            return false;
        }

        return true;
    }

    void print_usage(const char* program)
    {
        std::cout
        << "MCP Core " << VERSION << "\n\n"
        << "Usage:\n"
        << "  " << program << " --daemon [options]\n"
        << "  " << program << " --stdio [options]\n\n"
        << "Modes:\n"
        << "  --daemon           Run the MCP Core daemon\n"
        << "  --stdio            Run the STDIO-to-IPC bridge\n\n"
        << "Options:\n"
        << "  --plugin-dir PATH  Plugin directory (daemon only)\n"
        << "  --socket PATH      Unix domain socket path\n"
        << "  --help             Show this help\n"
        << "  --version          Show version\n\n"
        << "Defaults:\n"
        << "  Plugin directory:  " << DEFAULT_PLUGIN_DIR << '\n'
        << "  Socket:            " << DEFAULT_SOCKET << '\n';
    }

    int run_daemon(
        const std::string& plugin_directory,
        const std::string& socket_path
    )
    {
        std::cout
        << "========================================\n"
        << "          MCP CORE DAEMON "
        << VERSION << "\n"
        << "========================================\n";

        std::cout << "[MCP][INFO] Starting Core daemon\n";

        if (!install_signal_handlers()) {
            return 1;
        }

        std::error_code filesystem_error;

        if (!std::filesystem::is_directory(
            plugin_directory,
            filesystem_error)) {

            std::cerr
            << "[MCP][ERROR] Plugin directory unavailable: "
            << plugin_directory << '\n';

        if (filesystem_error) {
            std::cerr
            << "[MCP][ERROR] "
            << filesystem_error.message() << '\n';
        }

        return 1;
            }

            std::cout
            << "[MCP][INFO] Plugin directory: "
            << plugin_directory << '\n'
            << "[MCP][INFO] IPC socket: "
            << socket_path << '\n';

            mcp::PluginManager plugin_manager;

            std::cout << "[MCP][INFO] Loading plugins\n";

            if (!plugin_manager.load_directory(plugin_directory, "{}") ||
                plugin_manager.plugin_count() == 0) {

                std::cerr
                << "[MCP][ERROR] Plugin loading failed or no valid plugins "
                << "were loaded\n";

                plugin_manager.shutdown();
                return 1;
            }

            std::cout
            << "[MCP][INFO] Core initialization complete\n"
            << "[MCP][INFO] Loaded plugins: "
            << plugin_manager.plugin_count() << '\n'
            << "[MCP][INFO] Registered tools: "
            << plugin_manager.tool_count() << '\n';

            if (shutdown_requested != 0) {
                std::cout << "[MCP][INFO] Shutdown requested\n";
                plugin_manager.shutdown();
                return 0;
            }

            std::cout << "[MCP][INFO] Starting IPC server\n";

            mcp::IpcServer ipc_server(plugin_manager, socket_path);

            const int server_result =
            ipc_server.run(&shutdown_requested);

            std::cout << "[MCP][INFO] Shutting down plugins\n";

            plugin_manager.shutdown();

            if (server_result != 0) {
                std::cerr << "[MCP][ERROR] IPC server stopped with errors\n";
                return 1;
            }

            std::cout << "[MCP][INFO] Core shutdown complete\n";

            return 0;
    }

    int run_stdio(const std::string& socket_path)
    {
        std::cerr
        << "[MCP][INFO] Starting STDIO bridge\n"
        << "[MCP][INFO] IPC socket: "
        << socket_path << '\n';

        mcp::IpcClient ipc_client(socket_path);

        std::string error;

        if (!ipc_client.connect(error)) {
            std::cerr
            << "[MCP][ERROR] Cannot connect to Core daemon: "
            << error << '\n';

            return 1;
        }

        std::cerr << "[MCP][INFO] Connected to Core daemon\n";

        mcp::StdioBridge bridge(ipc_client);

        const int result = bridge.run(
            std::cin,
            std::cout,
            std::cerr
        );

        ipc_client.disconnect();

        std::cerr << "[MCP][INFO] STDIO bridge stopped\n";

        return result;
    }

} // namespace

int main(int argc, char* argv[])
{
    RunMode mode = RunMode::NONE;

    std::string plugin_directory = DEFAULT_PLUGIN_DIR;
    std::string socket_path = DEFAULT_SOCKET;

    bool plugin_directory_explicit = false;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if (argument == "--help") {
            print_usage(argv[0]);
            return 0;
        }

        if (argument == "--version") {
            std::cout << "MCP Core " << VERSION << '\n';
            return 0;
        }

        if (argument == "--daemon") {
            if (mode != RunMode::NONE) {
                std::cerr << "[MCP][ERROR] Select only one run mode\n";
                return 2;
            }

            mode = RunMode::DAEMON;
            continue;
        }

        if (argument == "--stdio") {
            if (mode != RunMode::NONE) {
                std::cerr << "[MCP][ERROR] Select only one run mode\n";
                return 2;
            }

            mode = RunMode::STDIO;
            continue;
        }

        if (argument == "--plugin-dir") {
            if (i + 1 >= argc) {
                std::cerr
                << "[MCP][ERROR] --plugin-dir requires a path\n";
                return 2;
            }

            plugin_directory = argv[++i];
            plugin_directory_explicit = true;
            continue;
        }

        if (argument == "--socket") {
            if (i + 1 >= argc) {
                std::cerr
                << "[MCP][ERROR] --socket requires a path\n";
                return 2;
            }

            socket_path = argv[++i];
            continue;
        }

        std::cerr
        << "[MCP][ERROR] Unknown argument: "
        << argument << '\n';

        print_usage(argv[0]);
        return 2;
    }

    if (mode == RunMode::NONE) {
        std::cerr
        << "[MCP][ERROR] A run mode is required "
        << "(--daemon or --stdio)\n";

        print_usage(argv[0]);
        return 2;
    }

    if (mode == RunMode::STDIO && plugin_directory_explicit) {
        std::cerr
        << "[MCP][ERROR] --plugin-dir is only valid with --daemon\n";
        return 2;
    }

    if (socket_path.empty()) {
        std::cerr << "[MCP][ERROR] Socket path cannot be empty\n";
        return 2;
    }

    if (mode == RunMode::DAEMON) {
        return run_daemon(plugin_directory, socket_path);
    }

    return run_stdio(socket_path);
}
