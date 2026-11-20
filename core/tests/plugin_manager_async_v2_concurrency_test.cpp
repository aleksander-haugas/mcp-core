#include "mcp/plugin_manager.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>

namespace {

constexpr int PRODUCERS = 8;
constexpr int MESSAGES_PER_PRODUCER = 100;
constexpr int EXPECTED_MESSAGES =
    PRODUCERS * MESSAGES_PER_PRODUCER;

struct TestState {
    std::array<std::array<bool, MESSAGES_PER_PRODUCER>, PRODUCERS>
        seen{};

    int received = 0;
    int malformed = 0;
    int duplicates = 0;
    int invalid_identity = 0;
};

int failures = 0;

void check(bool condition, const char* description)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ")
              << description << '\n';

    if (!condition) {
        ++failures;
    }
}

bool parse_payload(
    const std::vector<std::uint8_t>& data,
    int& producer,
    int& sequence
)
{
    const std::string payload(data.begin(), data.end());
    const std::size_t separator = payload.find(':');

    if (separator == std::string::npos ||
        separator == 0 ||
        separator + 1 >= payload.size()) {
        return false;
    }

    try {
        std::size_t first_end = 0;
        std::size_t second_end = 0;

        producer = std::stoi(payload.substr(0, separator), &first_end);
        sequence = std::stoi(payload.substr(separator + 1), &second_end);

        return first_end == separator &&
            second_end == payload.size() - separator - 1;
    } catch (...) {
        return false;
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0]
                  << " /path/to/plugin-directory\n";
        return 2;
    }

    TestState state;
    std::mutex mutex;
    std::condition_variable condition;

    mcp::PluginManager manager;

    manager.set_authenticated_message_handler(
        [&](const mcp::AuthenticatedPluginMessage& message) {
            {
                std::lock_guard<std::mutex> lock(mutex);

                ++state.received;

                if (message.plugin_id != "nakama" ||
                    message.transport_id != "nakama" ||
                    message.authenticated_principal !=
                        "concurrency-test-user") {
                    ++state.invalid_identity;
                }

                int producer = -1;
                int sequence = -1;

                if (!parse_payload(
                        message.data,
                        producer,
                        sequence) ||
                    producer < 0 ||
                    producer >= PRODUCERS ||
                    sequence < 0 ||
                    sequence >= MESSAGES_PER_PRODUCER) {
                    ++state.malformed;
                } else if (state.seen[producer][sequence]) {
                    ++state.duplicates;
                } else {
                    state.seen[producer][sequence] = true;
                }
            }

            condition.notify_one();
        }
    );

    check(
        manager.load_directory(argv[1], "{}"),
        "Concurrency fixture loaded"
    );

    check(
        manager.plugin_count() == 1 &&
        manager.tool_count() == 1,
        "Exactly one plugin and one tool registered"
    );

    std::string result;
    std::string error;

    const bool executed = manager.execute_tool(
        "nakama_test_concurrency",
        "{}",
        result,
        error
    );

    check(
        executed,
        executed ? "Concurrent producer tool executed"
                 : error.c_str()
    );

    check(
        result == "{\"published\":800}",
        "Plugin confirmed 800 successful publications"
    );

    bool all_received = false;

    {
        std::unique_lock<std::mutex> lock(mutex);

        all_received = condition.wait_for(
            lock,
            std::chrono::seconds(15),
            [&] {
                return state.received >= EXPECTED_MESSAGES;
            }
        );
    }

    check(
        all_received,
        "All 800 messages delivered within timeout"
    );

    {
        std::lock_guard<std::mutex> lock(mutex);

        int unique_messages = 0;

        for (const auto& producer : state.seen) {
            for (bool present : producer) {
                if (present) {
                    ++unique_messages;
                }
            }
        }

        check(
            state.received == EXPECTED_MESSAGES,
            "Exactly 800 callbacks received"
        );

        check(
            unique_messages == EXPECTED_MESSAGES,
            "All 800 producer/sequence pairs received"
        );

        check(
            state.duplicates == 0,
            "No duplicate messages"
        );

        check(
            state.malformed == 0,
            "No malformed or out-of-range payloads"
        );

        check(
            state.invalid_identity == 0,
            "Plugin, transport and principal preserved"
        );
    }

    manager.clear_authenticated_message_handler();
    manager.shutdown();

    check(
        manager.plugin_count() == 0 &&
        manager.tool_count() == 0,
        "Shutdown completed and registries are empty"
    );

    std::cout << "\nFailures: " << failures << '\n';

    if (failures == 0) {
        std::cout << "ASYNC V2 CONCURRENCY TEST: PASS\n";
        return 0;
    }

    std::cout << "ASYNC V2 CONCURRENCY TEST: FAIL\n";
    return 1;
}
