#include "../src/nakama_plugin.cpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int failures = 0;
int passed = 0;

void check(const char* name, bool condition)
{
    if (condition) {
        std::printf("PASS: %s\n", name);
        ++passed;
    } else {
        std::printf("FAIL: %s\n", name);
        ++failures;
    }
}

const std::string VALID_CONFIG =
    "{"
    "\"server_url\":\"wss://localhost:7350\","
    "\"server_key\":\"test-server-key\","
    "\"device_id\":\"test-device-id\","
    "\"connect_timeout_ms\":10000,"
    "\"verify_tls\":true"
    "}";

bool create_config(int base_fd, const std::string& contents,
                   mode_t mode = 0600)
{
    int fd = openat(
        base_fd,
        "mcp/nakama.json",
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
        0600
    );

    if (fd < 0) {
        return false;
    }

    size_t offset = 0;
    while (offset < contents.size()) {
        ssize_t count = write(
            fd,
            contents.data() + offset,
            contents.size() - offset
        );

        if (count < 0 && errno == EINTR) {
            continue;
        }

        if (count <= 0) {
            close(fd);
            return false;
        }

        offset += static_cast<size_t>(count);
    }

    const bool mode_ok = fchmod(fd, mode) == 0;
    const bool close_ok = close(fd) == 0;
    return mode_ok && close_ok;
}

void cleanup_mcp(int base_fd)
{
    unlinkat(base_fd, "mcp/nakama.json", 0);
    unlinkat(base_fd, "mcp/extra", 0);
    unlinkat(base_fd, "mcp", AT_REMOVEDIR);
}

ConfigState state(int base_fd)
{
    return load_config_state_at(base_fd, geteuid());
}

} // namespace

int main()
{
    char snapshot_template[] = "/tmp/mcp-nakama-snapshot-XXXXXX";
    int snapshot_fd = mkstemp(snapshot_template);

    if (snapshot_fd < 0) {
        std::perror("mkstemp snapshot");
        return 1;
    }

    struct stat initial_snapshot {};
    struct stat unchanged_snapshot {};
    bool snapshot_ok =
        fstat(snapshot_fd, &initial_snapshot) == 0 &&
        fstat(snapshot_fd, &unchanged_snapshot) == 0;

    check(
        "Unchanged file metadata snapshot accepted",
        snapshot_ok &&
        same_file_snapshot(initial_snapshot, unchanged_snapshot)
    );

    const char mutation[] = "changed";
    bool write_ok = write(
        snapshot_fd,
        mutation,
        sizeof(mutation)
    ) == static_cast<ssize_t>(sizeof(mutation));

    struct stat changed_snapshot {};
    bool changed_stat_ok =
        fstat(snapshot_fd, &changed_snapshot) == 0;

    check(
        "Modified file metadata snapshot rejected",
        write_ok &&
        changed_stat_ok &&
        !same_file_snapshot(initial_snapshot, changed_snapshot)
    );

    close(snapshot_fd);
    unlink(snapshot_template);
    std::string template_path = "/tmp/mcp-nakama-loader-XXXXXX";
    std::vector<char> path(
        template_path.begin(),
        template_path.end()
    );
    path.push_back('\0');

    if (mkdtemp(path.data()) == nullptr) {
        std::perror("mkdtemp");
        return 1;
    }

    int base_fd = open(
        path.data(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC
    );

    if (base_fd < 0) {
        std::perror("open temporary directory");
        rmdir(path.data());
        return 1;
    }

    check(
        "Missing configuration directory",
        state(base_fd) == ConfigState::MISSING
    );

    bool directory_ok = mkdirat(base_fd, "mcp", 0700) == 0;

    check("Create isolated configuration directory", directory_ok);

    if (!directory_ok) {
        close(base_fd);
        rmdir(path.data());
        return 1;
    }

    check(
        "Reject configuration directory with unexpected owner",
        load_config_state_at(
            base_fd,
            static_cast<uid_t>(geteuid() + 1)
        ) == ConfigState::INVALID
    );

    check(
        "Missing configuration file",
        state(base_fd) == ConfigState::MISSING
    );

    check(
        "Valid configuration file",
        create_config(base_fd, VALID_CONFIG) &&
        state(base_fd) == ConfigState::VALID
    );

    NakamaConfig loaded_config;

    const ConfigState loaded_state =
    load_config_state_at(
        base_fd,
        geteuid(),
                         &loaded_config
    );

    check(
        "Configuration values extracted correctly",
        loaded_state == ConfigState::VALID &&
        loaded_config.server_url == "wss://localhost:7350" &&
        loaded_config.server_key == "test-server-key" &&
        loaded_config.device_id == "test-device-id" &&
        loaded_config.connect_timeout_ms == 10000 &&
        loaded_config.verify_tls
    );

    cleanup_mcp(base_fd);
    mkdirat(base_fd, "mcp", 0700);

    NakamaConfig preserved_config;
    preserved_config.server_url = "wss://preserved.example:7350";
    preserved_config.server_key = "preserved-key";
    preserved_config.device_id = "preserved-device";
    preserved_config.connect_timeout_ms = 15000;
    preserved_config.verify_tls = true;

    const ConfigState invalid_state =
    create_config(base_fd, "{\"invalid\":true}")
    ? load_config_state_at(
        base_fd,
        geteuid(),
                           &preserved_config
    )
    : ConfigState::VALID;

    check(
        "Invalid configuration preserves output",
        invalid_state == ConfigState::INVALID &&
        preserved_config.server_url ==
        "wss://preserved.example:7350" &&
        preserved_config.server_key == "preserved-key" &&
        preserved_config.device_id == "preserved-device" &&
        preserved_config.connect_timeout_ms == 15000 &&
        preserved_config.verify_tls
    );

    cleanup_mcp(base_fd);
    mkdirat(base_fd, "mcp", 0700);

    check(
        "Reject configuration with mode 0644",
        create_config(base_fd, VALID_CONFIG, 0644) &&
        state(base_fd) == ConfigState::INVALID
    );
    cleanup_mcp(base_fd);
    mkdirat(base_fd, "mcp", 0700);

    check(
        "Reject FIFO without blocking",
        mkfifoat(base_fd, "mcp/nakama.json", 0600) == 0 &&
        state(base_fd) == ConfigState::INVALID
    );
    cleanup_mcp(base_fd);
    mkdirat(base_fd, "mcp", 0700);

    int target_fd = openat(
        base_fd,
        "target.json",
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
        0600
    );

    bool target_ok = target_fd >= 0;
    if (target_fd >= 0) {
        target_ok = write(
            target_fd,
            VALID_CONFIG.data(),
            VALID_CONFIG.size()
        ) == static_cast<ssize_t>(VALID_CONFIG.size());
        close(target_fd);
    }

    check(
        "Reject symbolic link",
        target_ok &&
        symlinkat("../target.json", base_fd, "mcp/nakama.json") == 0 &&
        state(base_fd) == ConfigState::INVALID
    );
    cleanup_mcp(base_fd);
    mkdirat(base_fd, "mcp", 0700);

    bool hardlink_ok = create_config(base_fd, VALID_CONFIG) &&
        linkat(
            base_fd, "mcp/nakama.json",
            base_fd, "mcp/extra", 0
        ) == 0;

    check(
        "Reject file with multiple hard links",
        hardlink_ok && state(base_fd) == ConfigState::INVALID
    );
    cleanup_mcp(base_fd);
    mkdirat(base_fd, "mcp", 0700);

    check(
        "Reject file larger than MAX_CONFIG_SIZE",
        create_config(
            base_fd,
            std::string(MAX_CONFIG_SIZE + 1, ' ')
        ) &&
        state(base_fd) == ConfigState::INVALID
    );

    cleanup_mcp(base_fd);
    unlinkat(base_fd, "target.json", 0);
    close(base_fd);
    rmdir(path.data());

    std::printf(
        "\nPassed: %d; Failures: %d\n",
        passed,
        failures
    );

    return failures == 0 ? 0 : 1;
}
