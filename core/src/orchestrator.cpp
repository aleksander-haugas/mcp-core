
#include "mcp/orchestrator.hpp"

#include "mcp/agent_protocol.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace mcp {

namespace {

using IdentityKey = std::pair<std::string, std::string>;

IdentityKey make_key(const AgentIdentity& identity) {
    return {identity.agent_id, identity.instance_id};
}

bool is_terminal(TaskState state) {
    return state == TaskState::COMPLETED ||
           state == TaskState::FAILED ||
           state == TaskState::TIMED_OUT;
}

} // namespace

class Orchestrator::Impl {
public:
    struct AgentRecord {
        AgentDescriptor descriptor;
        std::set<std::string> capabilities;
        std::size_t active_tasks = 0;
    };

    struct TaskRecord {
        TaskSnapshot snapshot;
        std::string payload_json;
        std::set<std::string> required_capabilities;
        std::chrono::milliseconds timeout{0};
    };

    Impl(
        AgentRegistry& registry_ref,
        AgentRpc& rpc_ref,
        const OrchestratorConfig& config_value
    )
        : registry(registry_ref),
          rpc(rpc_ref),
          config(config_value) {
    }

    bool start(std::string& error) {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
        std::unique_lock<std::mutex> lock(mutex);

        if (started) {
            error = "Orchestrator is already started";
            return false;
        }

        if (stopping) {
            error = "Orchestrator cannot be restarted after stop";
            return false;
        }

        if (config.worker_count == 0 ||
            config.max_queued_tasks == 0 ||
            config.default_agent_concurrency == 0 ||
            config.max_retained_tasks == 0) {
            error = "Orchestrator configuration contains zero limits";
        return false;
            }

            if (config.max_task_timeout.count() <= 0 ||
                config.max_task_timeout.count() >
                    static_cast<std::int64_t>(
                        std::numeric_limits<unsigned int>::max())) {
                error = "max_task_timeout is outside the supported range";
                return false;
            }

            if (config.default_timeout.count() <= 0 ||
                config.default_timeout > config.max_task_timeout) {
                error = "default_timeout exceeds max_task_timeout";
                return false;
            }

                    try {
                        workers.reserve(config.worker_count);

                        for (std::size_t i = 0; i < config.worker_count; ++i) {
                            workers.emplace_back([this]() {
                                worker_loop();
                            });
                        }

                        started = true;
                        error.clear();
                        return true;
                    } catch (...) {
                        stopping = true;
                        condition.notify_all();

                        // Workers need the mutex to observe stopping.
                        lock.unlock();

                        for (std::thread& worker : workers) {
                            if (worker.joinable()) {
                                worker.join();
                            }
                        }

                        lock.lock();
                        workers.clear();

                        error = "Failed to create orchestrator worker threads";
                        return false;
                    }
    }

    bool register_agent(
        const AgentDescriptor& descriptor,
        std::string& error
    ) {
        if (descriptor.identity.agent_id.empty() ||
            descriptor.identity.instance_id.empty()) {
            error = "Agent identity is incomplete";
            return false;
        }

        const std::size_t concurrency =
            descriptor.max_concurrent_tasks == 0
                ? config.default_agent_concurrency
                : descriptor.max_concurrent_tasks;

        if (concurrency == 0) {
            error = "Agent concurrency must be greater than zero";
            return false;
        }

        std::set<std::string> capabilities;

        for (const std::string& capability : descriptor.capabilities) {
            if (capability.empty() ||
                capability.size() > agent_protocol::MAX_METHOD_SIZE) {
                error = "Agent contains an invalid capability";
                return false;
            }

            if (!capabilities.insert(capability).second) {
                error = "Agent contains a duplicate capability";
                return false;
            }
        }

        AgentRegistration registration;

        if (!registry.resolve(descriptor.identity, registration)) {
            error = "Agent is not registered in AgentRegistry";
            return false;
        }

        if (!registration.active) {
            error = "Agent is not active in AgentRegistry";
            return false;
        }

        AgentDescriptor normalized = descriptor;
        normalized.max_concurrent_tasks = concurrency;

        {
            std::lock_guard<std::mutex> lock(mutex);

            if (stopping) {
                error = "Orchestrator is stopping";
                return false;
            }

            const IdentityKey key = make_key(descriptor.identity);

            if (agents.find(key) != agents.end()) {
                error = "Agent is already registered with Orchestrator";
                return false;
            }

            AgentRecord record;
            record.descriptor = std::move(normalized);
            record.capabilities = std::move(capabilities);

            agents.emplace(key, std::move(record));
        }

        condition.notify_all();
        error.clear();
        return true;
    }

    bool unregister_agent(
        const AgentIdentity& identity,
        std::string& error
    ) {
        {
            std::lock_guard<std::mutex> lock(mutex);

            const auto it = agents.find(make_key(identity));

            if (it == agents.end()) {
                error = "Agent is not registered with Orchestrator";
                return false;
            }

            if (it->second.active_tasks != 0) {
                error = "Agent has active tasks and cannot be unregistered";
                return false;
            }

            agents.erase(it);

            if (!agents.empty()) {
                round_robin_cursor %= agents.size();
            } else {
                round_robin_cursor = 0;
            }
        }

        condition.notify_all();
        error.clear();
        return true;
    }

    bool submit(
        const TaskRequest& request,
        std::string& task_id,
        std::string& error
    ) {
        if (request.method.empty() ||
            request.method.size() > agent_protocol::MAX_METHOD_SIZE) {
            error = "Task method is invalid";
            return false;
        }

        if (request.payload_json.empty() ||
            request.payload_json.size() > agent_protocol::MAX_MESSAGE_SIZE) {
            error = "Task payload is empty or exceeds the message limit";
            return false;
        }

        std::set<std::string> required;

        for (const std::string& capability : request.required_capabilities) {
            if (capability.empty() ||
                capability.size() > agent_protocol::MAX_METHOD_SIZE) {
                error = "Task contains an invalid required capability";
                return false;
            }

            required.insert(capability);
        }

        const std::chrono::milliseconds timeout =
            request.timeout.count() == 0
                ? config.default_timeout
                : request.timeout;

        if (timeout.count() <= 0 ||
            timeout > config.max_task_timeout) {
            error = "Task timeout exceeds max_task_timeout or is invalid";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);

            if (!started || stopping) {
                error = "Orchestrator is not accepting tasks";
                return false;
            }

            if (queue.size() >= config.max_queued_tasks) {
                error = "Orchestrator task queue is full";
                return false;
            }

            TaskRecord record;

            record.snapshot.task_id = next_task_id();
            record.snapshot.state = TaskState::PENDING;
            record.snapshot.method = request.method;
            record.snapshot.created_at =
                std::chrono::system_clock::now();
            record.snapshot.updated_at =
                record.snapshot.created_at;

            record.payload_json = request.payload_json;
            record.required_capabilities = std::move(required);
            record.timeout = timeout;

            task_id = record.snapshot.task_id;

            tasks.emplace(task_id, std::move(record));
            task_order.push_back(task_id);
            queue.push_back(task_id);
        }

        condition.notify_all();
        error.clear();
        return true;
    }

    bool get_task(
        const std::string& task_id,
        TaskSnapshot& snapshot
    ) const {
        std::lock_guard<std::mutex> lock(mutex);

        const auto it = tasks.find(task_id);

        if (it == tasks.end()) {
            return false;
        }

        snapshot = it->second.snapshot;
        return true;
    }

    std::vector<TaskSnapshot> list_tasks() const {
        std::vector<TaskSnapshot> result;

        std::lock_guard<std::mutex> lock(mutex);
        result.reserve(tasks.size());

        for (const auto& entry : tasks) {
            result.push_back(entry.second.snapshot);
        }

        std::sort(
            result.begin(),
            result.end(),
            [](const TaskSnapshot& lhs, const TaskSnapshot& rhs) {
                return lhs.created_at < rhs.created_at;
            }
        );

        return result;
    }

    void stop() noexcept {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
        {
            std::lock_guard<std::mutex> lock(mutex);

            if (stopping) {
                // Another caller may already be joining the workers.
                if (joining) {
                    return;
                }
            }

            stopping = true;
            started = false;

            for (const std::string& id : queue) {
                const auto it = tasks.find(id);

                if (it == tasks.end()) {
                    continue;
                }

                TaskRecord& task = it->second;

                if (task.snapshot.state == TaskState::PENDING) {
                    task.snapshot.state = TaskState::FAILED;
                    task.snapshot.error =
                        "Orchestrator stopped before task execution";
                    task.snapshot.updated_at =
                        std::chrono::system_clock::now();
                }
            }

            queue.clear();

            if (joining) {
                return;
            }

            joining = true;
        }

        condition.notify_all();

        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                try {
                    worker.join();
                } catch (...) {
                    // Destruction must remain noexcept.
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            workers.clear();
            joining = false;
        }
    }

private:
    std::string next_task_id() {
        ++task_counter;
        return "task-" + std::to_string(task_counter);
    }

    bool has_all_capabilities(
        const AgentRecord& agent,
        const std::set<std::string>& required
    ) const {
        for (const std::string& capability : required) {
            if (agent.capabilities.find(capability) ==
                agent.capabilities.end()) {
                return false;
            }
        }

        return true;
    }

    bool agent_is_active(const AgentIdentity& identity) const {
        AgentRegistration registration;

        return registry.resolve(identity, registration) &&
               registration.active;
    }

    bool select_agent_locked(
        const std::set<std::string>& required,
        AgentIdentity& selected
    ) {
        if (agents.empty()) {
            return false;
        }

        const std::size_t count = agents.size();
        auto it = agents.begin();

        std::advance(
            it,
            static_cast<std::ptrdiff_t>(
                round_robin_cursor % count
            )
        );

        for (std::size_t offset = 0; offset < count; ++offset) {
            if (it == agents.end()) {
                it = agents.begin();
            }

            AgentRecord& agent = it->second;

            const bool has_capacity =
                agent.active_tasks <
                agent.descriptor.max_concurrent_tasks;

            if (has_capacity &&
                has_all_capabilities(agent, required) &&
                agent_is_active(agent.descriptor.identity)) {
                selected = agent.descriptor.identity;
                ++agent.active_tasks;

                round_robin_cursor = (round_robin_cursor + offset + 1) % count;
                return true;
            }

            ++it;
        }

        return false;
    }

    void release_agent_locked(const AgentIdentity& identity) {
        const auto it = agents.find(make_key(identity));

        // An agent may have been unregistered while its RPC was active.
        if (it != agents.end() && it->second.active_tasks > 0) {
            --it->second.active_tasks;
        }
    }

    void prune_history_locked()
    {
        auto it_order = task_order.begin();

        while (tasks.size() > config.max_retained_tasks &&
            it_order != task_order.end()) {
            const auto it_task = tasks.find(*it_order);

        if (it_task == tasks.end()) {
            it_order = task_order.erase(it_order);
            continue;
        }

        if (!is_terminal(it_task->second.snapshot.state)) {
            // Preserve active tasks, but continue looking for
            // terminal records that can be pruned.
            ++it_order;
            continue;
        }

        tasks.erase(it_task);
        it_order = task_order.erase(it_order);
            }
    }

    bool take_task_locked(
        std::string& task_id,
        AgentIdentity& selected_agent
    ) {
        for (auto it = queue.begin(); it != queue.end(); ++it) {
            const auto task_it = tasks.find(*it);

            if (task_it == tasks.end() ||
                task_it->second.snapshot.state != TaskState::PENDING) {
                continue;
            }

            AgentIdentity candidate;

            if (!select_agent_locked(
                    task_it->second.required_capabilities,
                    candidate)) {
                continue;
            }

            task_id = *it;
            selected_agent = std::move(candidate);

            queue.erase(it);

            TaskRecord& task = task_it->second;
            task.snapshot.assigned_agent = selected_agent;
            task.snapshot.state = TaskState::ASSIGNED;
            task.snapshot.updated_at =
                std::chrono::system_clock::now();

            return true;
        }

        return false;
    }

    void worker_loop() noexcept {
        for (;;) {
            std::string task_id;
            AgentIdentity target;
            std::string method;
            std::string payload;
            unsigned int timeout_ms = 0;

            {
                std::unique_lock<std::mutex> lock(mutex);

                while (!stopping) {
                    if (take_task_locked(task_id, target)) {
                        auto it = tasks.find(task_id);

                        if (it != tasks.end()) {
                            TaskRecord& task = it->second;

                            task.snapshot.state = TaskState::RUNNING;
                            task.snapshot.updated_at =
                                std::chrono::system_clock::now();

                            method = task.snapshot.method;
                            payload = task.payload_json;
                            timeout_ms = static_cast<unsigned int>(
                                task.timeout.count()
                            );
                        }

                        break;
                    }

                    // Periodic revalidation also detects external changes
                    // to AgentRegistry::active.
                    condition.wait_for(
                        lock,
                        std::chrono::milliseconds(250)
                    );
                }

                if (stopping) {
                    return;
                }
            }

            std::string result_json;
            std::string rpc_error;
            bool success = false;

            try {
                success = rpc.call(
                    target,
                    method,
                    payload,
                    result_json,
                    rpc_error,
                    timeout_ms
                );
            } catch (...) {
                rpc_error = "AgentRpc::call threw an exception";
            }

            {
                std::lock_guard<std::mutex> lock(mutex);

                release_agent_locked(target);

                const auto it = tasks.find(task_id);

                if (it != tasks.end()) {
                    TaskRecord& task = it->second;

                    if (success) {
                        task.snapshot.state = TaskState::COMPLETED;
                        task.snapshot.result_json = std::move(result_json);
                        task.snapshot.error.clear();
                    } else {
                        const bool timed_out =
                            rpc_error == "RPC request timed out";

                        task.snapshot.error = rpc_error.empty()
                            ? "Agent RPC failed"
                            : std::move(rpc_error);

                        task.snapshot.state = timed_out
                            ? TaskState::TIMED_OUT
                            : TaskState::FAILED;
                    }

                    task.snapshot.updated_at =
                        std::chrono::system_clock::now();
                }

                prune_history_locked();
            }

            condition.notify_all();
        }
    }

    AgentRegistry& registry;
    AgentRpc& rpc;
    OrchestratorConfig config;

    // Serializes start() and stop() across concurrent callers.
    std::mutex lifecycle_mutex;

    mutable std::mutex mutex;
    std::condition_variable condition;

    std::map<IdentityKey, AgentRecord> agents;
    std::map<std::string, TaskRecord> tasks;

    std::deque<std::string> queue;
    std::deque<std::string> task_order;

    std::vector<std::thread> workers;

    std::size_t round_robin_cursor = 0;
    std::uint64_t task_counter = 0;

    bool started = false;
    bool stopping = false;
    bool joining = false;
};

Orchestrator::Orchestrator(
    AgentRegistry& registry,
    AgentRpc& rpc,
    const OrchestratorConfig& config
)
    : impl_(new Impl(registry, rpc, config)) {
}

Orchestrator::~Orchestrator() {
    stop();
}

bool Orchestrator::start(std::string& error) {
    return impl_->start(error);
}

bool Orchestrator::register_agent(
    const AgentDescriptor& descriptor,
    std::string& error
) {
    return impl_->register_agent(descriptor, error);
}

bool Orchestrator::unregister_agent(
    const AgentIdentity& identity,
    std::string& error
) {
    return impl_->unregister_agent(identity, error);
}

bool Orchestrator::submit(
    const TaskRequest& request,
    std::string& task_id,
    std::string& error
) {
    return impl_->submit(request, task_id, error);
}

bool Orchestrator::get_task(
    const std::string& task_id,
    TaskSnapshot& snapshot
) const {
    return impl_->get_task(task_id, snapshot);
}

std::vector<TaskSnapshot> Orchestrator::list_tasks() const {
    return impl_->list_tasks();
}

void Orchestrator::stop() noexcept {
    if (impl_) {
        impl_->stop();
    }
}

} // namespace mcp

