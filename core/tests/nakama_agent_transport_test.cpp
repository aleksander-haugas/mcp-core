
#include "mcp/agent_bus.hpp"
#include "mcp/nakama_agent_transport.hpp"
#include "mcp/plugin_manager.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

mcp::AgentMessage make_message(
    const mcp::AgentIdentity& source,
    const mcp::AgentIdentity& target,
    const std::string& message_id,
    const std::string& payload
)
{
    mcp::AgentMessage message;

    message.type = mcp::AgentMessageType::REQUEST;
    message.message_id = message_id;
    message.source = source;
    message.target = target;
    message.has_target = true;
    message.method = "test.echo";
    message.has_method = true;
    message.payload_json = payload;
    message.timestamp = "2026-11-22T12:00:00Z";

    return message;
}

std::string serialize(const mcp::AgentMessage& message)
{
    std::string output;
    std::string error;

    require(
        mcp::agent_protocol::serialize_message(
            message,
            output,
            error
        ),
        "Message serialization failed: " + error
    );

    return output;
}

std::string execute(
    mcp::PluginManager& manager,
    const std::string& tool
)
{
    std::string result;
    std::string error;

    require(
        manager.execute_tool(tool, "{}", result, error),
        "Plugin tool failed (" + tool + "): " + error
    );

    return result;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        require(argc == 2, "Expected fixture plugin directory");

        const mcp::AgentIdentity remote_identity{
            "remote-agent",
            "instance-1"
        };

        const mcp::AgentIdentity core_identity{
            "core-agent",
            "core-instance"
        };

        mcp::PluginManager manager;

        require(
            manager.load_directory(argv[1], "{}"),
            "PluginManager failed to load fixture directory"
        );

        require(
            manager.plugin_count() == 1,
            "Expected exactly one fixture plugin"
        );

        require(
            manager.tool_count() == 2,
            "Expected exactly two fixture tools"
        );

        mcp::AgentRegistry registry;
        std::string error;

        mcp::AgentRegistration remote;
        remote.identity = remote_identity;
        remote.principal_id = "nakama-user-42";
        remote.transport_id = "nakama";
        remote.active = true;

        require(
            registry.register_agent(remote, error),
            "Remote agent registration failed: " + error
        );

        mcp::AgentRegistration core;
        core.identity = core_identity;
        core.principal_id = "core-principal";
        core.transport_id = "nakama";
        core.active = true;

        require(
            registry.register_agent(core, error),
            "Core agent registration failed: " + error
        );

        mcp::AgentBus bus(registry);

        std::mutex received_mutex;
        std::condition_variable received_cv;
        bool inbound_received = false;
        std::string inbound_payload;

        bus.set_message_handler(
            [&](const mcp::AgentRegistration& source,
                const mcp::AgentMessage& message) {
                std::lock_guard<std::mutex> lock(received_mutex);

                if (source.principal_id == "nakama-user-42" &&
                    message.message_id == "fixture-inbound-001") {
                    inbound_payload = message.payload_json;
                    inbound_received = true;
                }

                received_cv.notify_all();
            }
        );

        auto transport =
            std::make_shared<mcp::NakamaAgentTransport>(manager);

        require(
            bus.add_transport(transport, error),
            "Unable to add NakamaAgentTransport: " + error
        );

        // Test 1: Plugin -> authenticated Core -> AgentBus.
        const std::string publish_result =
            execute(manager, "nakama_test_publish");

        require(
            publish_result == "{\"published\":true}",
            "Unexpected publish tool result"
        );

        {
            std::unique_lock<std::mutex> lock(received_mutex);

            const bool received = received_cv.wait_for(
                lock,
                std::chrono::seconds(5),
                [&] {
                    return inbound_received;
                }
            );

            require(
                received,
                "AgentBus did not receive the authenticated message"
            );
        }

        require(
            inbound_payload == "{\"input\":42}",
            "Inbound payload did not match"
        );

        std::cout
            << "[PASS] Authenticated plugin -> AgentBus delivery\n";

        // Test 2: Core -> AgentBus -> plugin async receiver.
        const auto outbound = make_message(
            core_identity,
            remote_identity,
            "core-outbound-001",
            "{\"output\":84}"
        );

        const std::string outbound_json = serialize(outbound);

        require(
            bus.send(
                "core-principal",
                outbound_json,
                error
            ),
            "AgentBus outbound send failed: " + error
        );

        const std::string received_json =
            execute(manager, "nakama_test_get_received");

        require(
            received_json == outbound_json,
            "Plugin receiver did not receive the exact outbound message"
        );

        std::cout
            << "[PASS] AgentBus -> plugin async receiver delivery\n";

        // Test 3: orderly transport removal and callback drain.
        require(
            bus.remove_transport("nakama", error),
            "Unable to remove Nakama transport: " + error
        );

        std::cout
            << "[PASS] Transport removal and callback drain\n";

        // The transport is stopped before PluginManager shutdown.
        bus.stop();
        manager.shutdown();

        std::cout << "All NakamaAgentTransport tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "TEST FAILURE: " << exception.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "TEST FAILURE: unknown exception\n";
        return 1;
    }
}
