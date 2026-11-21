
#pragma once

#include "mcp/agent_registry.hpp"
#include "mcp/agent_rpc.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mcp {

enum class TaskState {
    PENDING,
    ASSIGNED,
    RUNNING,
    COMPLETED,
    FAILED,
    TIMED_OUT
};

struct OrchestratorConfig {
    // Maximum number of tasks waiting for execution.
    std::size_t max_queued_tasks = 256;

    // Independent workers executing RPC calls.
    std::size_t worker_count = 4;

    // Maximum number of concurrently executing tasks per agent.
    std::size_t default_agent_concurrency = 1;

    // Maximum number of completed/failed task records retained.
    std::size_t max_retained_tasks = 2048;

    // Default RPC timeout.
    std::chrono::milliseconds default_timeout{5000};
    // Maximum permitted timeout for an individual task.
    std::chrono::milliseconds max_task_timeout{300000};
};

struct AgentDescriptor {
    AgentIdentity identity;
    std::vector<std::string> capabilities;
    std::size_t max_concurrent_tasks = 0;
};

struct TaskRequest {
    // RPC method exposed by the target agent.
    std::string method;

    // JSON payload passed unchanged to AgentRpc::call.
    std::string payload_json = "{}";

    // All requested capabilities must be supported.
    std::vector<std::string> required_capabilities;

    // Zero means use the configured default timeout.
    std::chrono::milliseconds timeout{0};
};

struct TaskSnapshot {
    std::string task_id;
    TaskState state = TaskState::PENDING;

    std::string method;
    AgentIdentity assigned_agent;

    std::string result_json;
    std::string error;

    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
};

class Orchestrator {
public:
    Orchestrator(
        AgentRegistry& registry,
        AgentRpc& rpc,
        const OrchestratorConfig& config = OrchestratorConfig{}
    );

    ~Orchestrator();

    Orchestrator(const Orchestrator&) = delete;
    Orchestrator& operator=(const Orchestrator&) = delete;

    // Starts the worker pool.
    bool start(std::string& error);

    // Registers orchestration metadata for an already registered agent.
    bool register_agent(
        const AgentDescriptor& descriptor,
        std::string& error
    );

    bool unregister_agent(
        const AgentIdentity& identity,
        std::string& error
    );

    // Accepts a task and returns its generated identifier.
    bool submit(
        const TaskRequest& request,
        std::string& task_id,
        std::string& error
    );

    // Returns a consistent snapshot of a task.
    bool get_task(
        const std::string& task_id,
        TaskSnapshot& snapshot
    ) const;

    // Returns retained task snapshots.
    std::vector<TaskSnapshot> list_tasks() const;

    // Stops accepting work and joins the orchestrator workers.
    void stop() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcp

