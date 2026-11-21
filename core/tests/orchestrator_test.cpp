#include "mcp/orchestrator.hpp"

#include "mcp/agent_bus.hpp"
#include "mcp/agent_rpc.hpp"
#include "mcp/agent_transport.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Packet {
    std::string principal;
    std::string payload;
};

struct Endpoint {
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Packet> queue;
    mcp::AgentTransport::MessageHandler handler;
    bool stopping = false;
};

struct GateState {
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t entered = 0;
    std::size_t active = 0;
    std::size_t max_active = 0;
    bool released = false;
};

bool wait_for_entries(
    GateState& gate,
    std::size_t expected,
    std::chrono::milliseconds timeout
)
{
    std::unique_lock<std::mutex> lock(gate.mutex);
    return gate.condition.wait_for(lock, timeout, [&]() {
        return gate.entered >= expected;
    });
}

void release_gate(GateState& gate)
{
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.released = true;
    }
    gate.condition.notify_all();
}

mcp::AgentRpc::MethodHandler make_gated_handler(
    const std::shared_ptr<GateState>& gate,
    const std::string& result_value
)
{
    return [gate, result_value](
        const mcp::AgentIdentity&,
        const std::string&,
        std::string& result,
        std::string&
    ) {
        std::unique_lock<std::mutex> lock(gate->mutex);
        ++gate->entered;
        ++gate->active;

        if (gate->active > gate->max_active) {
            gate->max_active = gate->active;
        }

        gate->condition.notify_all();

        gate->condition.wait(lock, [&]() {
            return gate->released;
        });

        --gate->active;
        result = "{\"result\":\"" + result_value + "\"}";
        return true;
    };
}

class FakeTransport final : public mcp::AgentTransport {
public:
    FakeTransport(
        std::string principal,
        std::string transport_id,
        std::shared_ptr<Endpoint> local,
        std::shared_ptr<Endpoint> peer
    )
    : principal_(std::move(principal)),
    transport_id_(std::move(transport_id)),
    local_(std::move(local)),
    peer_(std::move(peer))
    {
    }

    ~FakeTransport() override
    {
        stop();
    }

    std::string transport_id() const override
    {
        return transport_id_;
    }

    bool start(
        MessageHandler handler,
        std::string& error
    ) override
    {
        {
            std::lock_guard<std::mutex> lock(local_->mutex);

            if (!workers_.empty() || local_->stopping) {
                error = "FakeTransport cannot be started";
                return false;
            }

            local_->handler = std::move(handler);
        }

        try {
            // Dos workers permiten procesar una segunda RPC mientras
            // otro handler permanece bloqueado.
            for (int i = 0; i < 2; ++i) {
                workers_.emplace_back([this]() {
                    run();
                });
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(local_->mutex);
                local_->stopping = true;
            }

            local_->condition.notify_all();

            for (auto& worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }

            workers_.clear();

            error = "Unable to start FakeTransport workers";
            return false;
        }

        error.clear();
        return true;
    }

    bool send(
        const mcp::AgentRegistration&,
        const std::string& message_json,
        std::string& error
    ) override
    {
        {
            std::lock_guard<std::mutex> lock(peer_->mutex);

            if (peer_->stopping || !peer_->handler) {
                error = "FakeTransport peer is unavailable";
                return false;
            }

            if (peer_->queue.size() >= 64) {
                error = "FakeTransport queue is full";
                return false;
            }

            peer_->queue.push_back({
                principal_,
                message_json
            });
        }

        peer_->condition.notify_one();
        error.clear();
        return true;
    }

    void stop() noexcept override
    {
        {
            std::lock_guard<std::mutex> lock(local_->mutex);
            local_->stopping = true;
        }

        local_->condition.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable() &&
                worker.get_id() != std::this_thread::get_id()) {
                worker.join();
                }
        }

        workers_.clear();
    }

private:
    void run() noexcept
    {
        for (;;) {
            Packet packet;
            MessageHandler handler;

            {
                std::unique_lock<std::mutex> lock(local_->mutex);

                local_->condition.wait(lock, [this]() {
                    return local_->stopping || !local_->queue.empty();
                });

                if (local_->queue.empty() && local_->stopping) {
                    return;
                }

                packet = std::move(local_->queue.front());
                local_->queue.pop_front();
                handler = local_->handler;
            }

            if (handler) {
                try {
                    handler(packet.principal, packet.payload);
                } catch (...) {
                    // No propagar excepciones al worker.
                }
            }
        }
    }

    std::string principal_;
    std::string transport_id_;
    std::shared_ptr<Endpoint> local_;
    std::shared_ptr<Endpoint> peer_;
    std::vector<std::thread> workers_;
};

struct TransportPair {
    std::shared_ptr<FakeTransport> first;
    std::shared_ptr<FakeTransport> second;
};

TransportPair make_transport_pair()
{
    auto first_endpoint = std::make_shared<Endpoint>();
    auto second_endpoint = std::make_shared<Endpoint>();

    return {
        std::make_shared<FakeTransport>(
            "principal-a",
            "orchestrator-test",
            first_endpoint,
            second_endpoint
        ),
        std::make_shared<FakeTransport>(
            "principal-b",
            "orchestrator-test",
            second_endpoint,
            first_endpoint
        )
    };
}

bool wait_for_terminal(
    mcp::Orchestrator& orchestrator,
    const std::string& task_id,
    mcp::TaskSnapshot& snapshot,
    std::chrono::milliseconds timeout
)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (orchestrator.get_task(task_id, snapshot)) {
            if (snapshot.state == mcp::TaskState::COMPLETED ||
                snapshot.state == mcp::TaskState::FAILED ||
                snapshot.state == mcp::TaskState::TIMED_OUT) {
                return true;
            }
        }

        std::this_thread::sleep_for(2ms);
    }

    return false;
}

void run_tests()
{
    const mcp::AgentIdentity orchestrator_identity{
        "orchestrator",
        "instance-a"
    };

    const mcp::AgentIdentity executor_identity{
        "executor",
        "instance-b"
    };

    mcp::AgentRegistry registry;
    std::string error;

    require(
        registry.register_agent({
            orchestrator_identity,
            "principal-a",
            "orchestrator-test",
            true
        }, error),
        "Unable to register orchestrator identity: " + error
    );

    require(
        registry.register_agent({
            executor_identity,
            "principal-b",
            "orchestrator-test",
            true
        }, error),
        "Unable to register executor identity: " + error
    );

    mcp::AgentBus bus_a(registry);
    mcp::AgentBus bus_b(registry);

    auto transports = make_transport_pair();

    require(
        bus_a.add_transport(transports.first, error),
        "Unable to add transport A: " + error
    );

    require(
        bus_b.add_transport(transports.second, error),
        "Unable to add transport B: " + error
    );

    mcp::AgentRpc rpc_a(
        bus_a,
        orchestrator_identity,
        "principal-a"
    );

    mcp::AgentRpc rpc_b(
        bus_b,
        executor_identity,
        "principal-b"
    );

    std::atomic<bool> slow_finished{false};

    auto concurrency_gate = std::make_shared<GateState>();
    auto shutdown_gate = std::make_shared<GateState>();
    auto history_gate = std::make_shared<GateState>();

    require(
        rpc_b.register_method(
            "test.gated.concurrency",
            make_gated_handler(concurrency_gate, "concurrency-ok"),
            error
        ),
        "Unable to register concurrency gate: " + error
    );

    require(
        rpc_b.register_method(
            "test.gated.shutdown",
            make_gated_handler(shutdown_gate, "shutdown-ok"),
            error
        ),
        "Unable to register shutdown gate: " + error
    );

    require(
        rpc_b.register_method(
            "test.echo",
            [](
                const mcp::AgentIdentity&,
                const std::string&,
                std::string& result,
                std::string&
            ) {
                result = "{\"result\":\"echo-ok\"}";
                return true;
            },
            error
        ),
        "Unable to register echo method: " + error
    );

    require(
        rpc_b.register_method(
            "test.gated.history",
            make_gated_handler(history_gate, "history-ok"),
                              error
        ),
        "Unable to register history gate: " + error
    );

    require(
        rpc_b.register_method(
            "test.fail",
            [](
                const mcp::AgentIdentity&,
                const std::string&,
                std::string&,
                std::string& handler_error
            ) {
                handler_error = "intentional remote failure";
                return false;
            },
            error
        ),
        "Unable to register fail method: " + error
    );

    require(
        rpc_b.register_method(
            "test.slow",
            [&slow_finished](
                const mcp::AgentIdentity&,
                const std::string&,
                std::string& result,
                std::string&
            ) {
                std::this_thread::sleep_for(200ms);
                result = "{\"result\":\"slow-ok\"}";
                slow_finished.store(true);
                return true;
            },
            error
        ),
        "Unable to register slow method: " + error
    );

    // Invalid configuration: default timeout exceeds the maximum.
    {
        mcp::OrchestratorConfig invalid_config;
        invalid_config.default_timeout =
            std::chrono::milliseconds(300001);
        invalid_config.max_task_timeout =
            std::chrono::milliseconds(300000);

        mcp::Orchestrator invalid_orchestrator(
            registry,
            rpc_a,
            invalid_config
        );

        require(
            !invalid_orchestrator.start(error),
            "Orchestrator accepted default_timeout above max_task_timeout"
        );

        require(
            error == "default_timeout exceeds max_task_timeout",
            "Unexpected default timeout validation error: " + error
        );

        std::cout << "[PASS] Default timeout configuration validation\n";
    }

    mcp::OrchestratorConfig config;
    config.worker_count = 2;
    config.max_queued_tasks = 16;
    config.default_timeout = 1000ms;

    mcp::Orchestrator orchestrator(registry, rpc_a, config);

    mcp::AgentDescriptor executor;
    executor.identity = executor_identity;
    executor.capabilities = {"test.echo", "test.fail", "test.slow"};
    executor.max_concurrent_tasks = 2;

    require(
        orchestrator.register_agent(executor, error),
        "Unable to register executor with Orchestrator: " + error
    );

    require(
        orchestrator.start(error),
        "Unable to start Orchestrator: " + error
    );

    // Test 1: successful RPC.
    {
        mcp::TaskRequest request;
        request.method = "test.echo";
        request.payload_json = "{\"input\":42}";
        request.required_capabilities = {"test.echo"};

        std::string task_id;

        require(
            orchestrator.submit(request, task_id, error),
            "Unable to submit echo task: " + error
        );

        mcp::TaskSnapshot snapshot;

        require(
            wait_for_terminal(
                orchestrator,
                task_id,
                snapshot,
                3000ms
            ),
            "Echo task did not reach a terminal state"
        );

        require(
            snapshot.state == mcp::TaskState::COMPLETED,
            "Echo task did not complete successfully: " + snapshot.error
        );

        require(
            snapshot.result_json == "{\"result\":\"echo-ok\"}",
            "Unexpected echo result: " + snapshot.result_json
        );

        std::cout << "[PASS] Successful RPC execution\n";
    }

    // Test 2: remote method failure.
    {
        mcp::TaskRequest request;
        request.method = "test.fail";
        request.required_capabilities = {"test.fail"};

        std::string task_id;

        require(
            orchestrator.submit(request, task_id, error),
            "Unable to submit failure task: " + error
        );

        mcp::TaskSnapshot snapshot;

        require(
            wait_for_terminal(
                orchestrator,
                task_id,
                snapshot,
                3000ms
            ),
            "Failure task did not reach a terminal state"
        );

        require(
            snapshot.state == mcp::TaskState::FAILED,
            "Remote failure was not classified as FAILED"
        );

        require(
            snapshot.error.find("intentional remote failure") !=
                std::string::npos,
            "Remote error message was not preserved: " + snapshot.error
        );

        std::cout << "[PASS] Remote RPC failure\n";
    }

    // Test 3: local RPC timeout.
    {
        mcp::TaskRequest request;
        request.method = "test.slow";
        request.required_capabilities = {"test.slow"};
        request.timeout = 40ms;

        std::string task_id;

        require(
            orchestrator.submit(request, task_id, error),
            "Unable to submit timeout task: " + error
        );

        mcp::TaskSnapshot snapshot;

        require(
            wait_for_terminal(
                orchestrator,
                task_id,
                snapshot,
                3000ms
            ),
            "Timeout task did not reach a terminal state"
        );

        require(
            snapshot.state == mcp::TaskState::TIMED_OUT,
            "RPC timeout was not classified as TIMED_OUT: " +
                snapshot.error
        );

        std::cout << "[PASS] RPC timeout classification\n";
    }

    // Wait for the remote slow handler to finish before teardown.
    const auto slow_deadline =
        std::chrono::steady_clock::now() + 3000ms;

    while (!slow_finished.load() &&
           std::chrono::steady_clock::now() < slow_deadline) {
        std::this_thread::sleep_for(2ms);
    }

    require(
        slow_finished.load(),
        "Remote slow handler did not finish"
    );

    orchestrator.stop();

    // Segunda fase: límites de concurrencia, cola, lifecycle e historial.
    mcp::OrchestratorConfig advanced_config;
    advanced_config.worker_count = 2;
    advanced_config.max_queued_tasks = 1;
    advanced_config.default_agent_concurrency = 1;
    advanced_config.max_retained_tasks = 2;
    advanced_config.default_timeout = 2000ms;

    mcp::Orchestrator advanced(
        registry,
        rpc_a,
        advanced_config
    );

    mcp::AgentDescriptor default_concurrency_executor;
    default_concurrency_executor.identity = executor_identity;
    default_concurrency_executor.capabilities = {
        "test.echo",
        "test.gated.concurrency",
        "test.gated.shutdown"
    };
    // 0 debe resolver a default_agent_concurrency (1).
    default_concurrency_executor.max_concurrent_tasks = 0;

    require(
        advanced.register_agent(default_concurrency_executor, error),
        "Unable to register advanced executor: " + error
    );

    require(
        advanced.start(error),
        "Unable to start advanced Orchestrator: " + error
    );

    // Reject task timeouts above the configured five-minute maximum.
    {
        mcp::TaskRequest request;
        request.method = "test.echo";
        request.required_capabilities = {"test.echo"};
        request.timeout = std::chrono::milliseconds(300001);

        std::string task_id;

        require(
            !advanced.submit(request, task_id, error),
            "Orchestrator accepted a timeout above five minutes"
        );

        require(
            error ==
                "Task timeout exceeds max_task_timeout or is invalid",
            "Unexpected maximum timeout validation error: " + error
        );

        std::cout << "[PASS] Maximum task timeout enforcement\n";
    }

    // Test 4: concurrencia predeterminada = 1.
    std::string concurrency_task_a;
    std::string concurrency_task_b;

    mcp::TaskRequest concurrency_request;
    concurrency_request.method = "test.gated.concurrency";
    concurrency_request.required_capabilities = {
        "test.gated.concurrency"
    };

    require(
        advanced.submit(
            concurrency_request,
            concurrency_task_a,
            error
        ),
        "Unable to submit first concurrency task: " + error
    );

    require(
        wait_for_entries(*concurrency_gate, 1, 3000ms),
        "First concurrency handler did not start"
    );

    require(
        advanced.submit(
            concurrency_request,
            concurrency_task_b,
            error
        ),
        "Unable to submit second concurrency task: " + error
    );

    mcp::TaskSnapshot concurrency_snapshot_b;

    require(
        advanced.get_task(concurrency_task_b, concurrency_snapshot_b),
        "Unable to retrieve second concurrency task"
    );

    require(
        concurrency_snapshot_b.state == mcp::TaskState::PENDING,
        "Second task ran despite the default concurrency limit"
    );

    {
        std::lock_guard<std::mutex> lock(concurrency_gate->mutex);
        require(
            concurrency_gate->max_active == 1,
            "Agent exceeded its default concurrency limit"
        );
    }

    release_gate(*concurrency_gate);

    mcp::TaskSnapshot concurrency_snapshot_a;

    require(
        wait_for_terminal(
            advanced,
            concurrency_task_a,
            concurrency_snapshot_a,
            3000ms
        ),
        "First concurrency task did not finish"
    );

    require(
        wait_for_terminal(
            advanced,
            concurrency_task_b,
            concurrency_snapshot_b,
            3000ms
        ),
        "Second concurrency task did not finish"
    );

    require(
        concurrency_snapshot_a.state == mcp::TaskState::COMPLETED &&
        concurrency_snapshot_b.state == mcp::TaskState::COMPLETED,
        "Concurrency test tasks did not both complete"
    );

    {
        std::lock_guard<std::mutex> lock(concurrency_gate->mutex);
        require(
            concurrency_gate->max_active == 1,
            "Observed more than one concurrent handler"
        );
    }

    std::cout << "[PASS] Default agent concurrency limit\n";

    // Test 5: poda del historial con una tarea antigua activa.
    {
        mcp::OrchestratorConfig history_config;
        history_config.worker_count = 2;
        history_config.max_queued_tasks = 8;
        history_config.default_agent_concurrency = 2;
        history_config.max_retained_tasks = 2;
        history_config.default_timeout = 2000ms;

        mcp::Orchestrator history_orchestrator(
            registry,
            rpc_a,
            history_config
        );

        mcp::AgentDescriptor history_executor;
        history_executor.identity = executor_identity;
        history_executor.capabilities = {
            "test.echo",
            "test.gated.history"
        };
        history_executor.max_concurrent_tasks = 2;

        require(
            history_orchestrator.register_agent(
                history_executor,
                error
            ),
            "Unable to register history executor: " + error
        );

        require(
            history_orchestrator.start(error),
                "Unable to start history Orchestrator: " + error
        );

        std::string blocked_task_id;
        mcp::TaskRequest blocked_request;
        blocked_request.method = "test.gated.history";
        blocked_request.required_capabilities = {
            "test.gated.history"
        };

        require(
            history_orchestrator.submit(
                blocked_request,
                blocked_task_id,
                error
            ),
            "Unable to submit blocked history task: " + error
        );

        require(
            wait_for_entries(*history_gate, 1, 3000ms),
                "History gate handler did not start"
        );

        std::vector<std::string> completed_ids;

        for (int index = 0; index < 3; ++index) {
            mcp::TaskRequest request;
            request.method = "test.echo";
            request.required_capabilities = {"test.echo"};

            std::string task_id;

            require(
                history_orchestrator.submit(request, task_id, error),
                    "Unable to submit history task: " + error
            );

            mcp::TaskSnapshot snapshot;

            require(
                wait_for_terminal(
                    history_orchestrator,
                    task_id,
                    snapshot,
                    3000ms
                ),
                "History task did not finish"
            );

            require(
                snapshot.state == mcp::TaskState::COMPLETED,
                "History task failed: " + snapshot.error
            );

            completed_ids.push_back(task_id);
        }

        const std::vector<mcp::TaskSnapshot> retained =
        history_orchestrator.list_tasks();

        bool blocked_task_retained = false;

        for (const auto& snapshot : retained) {
            if (snapshot.task_id == blocked_task_id &&
                snapshot.state == mcp::TaskState::RUNNING) {
                blocked_task_retained = true;
                }
        }

        // Liberar antes de cualquier comprobación que pueda lanzar.
        release_gate(*history_gate);

        mcp::TaskSnapshot blocked_snapshot;

        const bool blocked_finished = wait_for_terminal(
            history_orchestrator,
            blocked_task_id,
            blocked_snapshot,
            3000ms
        );

        const std::vector<mcp::TaskSnapshot> final_retained =
        history_orchestrator.list_tasks();

        require(
            blocked_task_retained,
            "History pruning removed an active task"
        );

        require(
            retained.size() <= history_config.max_retained_tasks,
                "Task history exceeded max_retained_tasks while an older task was active"
        );

        require(
            blocked_finished &&
            blocked_snapshot.state == mcp::TaskState::COMPLETED,
            "Blocked history task did not complete"
        );

        require(
            final_retained.size() <= history_config.max_retained_tasks,
                "Task history exceeded max_retained_tasks after gate completion"
        );

        std::cout << "[PASS] Terminal task pruning with active older task\n";
    }

    const std::vector<mcp::TaskSnapshot> retained =
        advanced.list_tasks();

    require(
        retained.size() <= advanced_config.max_retained_tasks,
        "Task history exceeded max_retained_tasks"
    );

    std::cout << "[PASS] Terminal task history pruning\n";

    // Test 6: cola llena, unregister protegido y stop con tarea activa.
    std::string active_task_id;
    std::string pending_task_id;
    std::string rejected_task_id;

    mcp::TaskRequest shutdown_request;
    shutdown_request.method = "test.gated.shutdown";
    shutdown_request.required_capabilities = {
        "test.gated.shutdown"
    };

    require(
        advanced.submit(shutdown_request, active_task_id, error),
        "Unable to submit active shutdown task: " + error
    );

    require(
        wait_for_entries(*shutdown_gate, 1, 3000ms),
        "Shutdown handler did not start"
    );

    mcp::TaskRequest queued_request;
    queued_request.method = "test.echo";
    queued_request.required_capabilities = {"test.echo"};

    require(
        advanced.submit(queued_request, pending_task_id, error),
        "Unable to submit pending queue task: " + error
    );

    require(
        !advanced.submit(queued_request, rejected_task_id, error),
        "Orchestrator accepted a task beyond its queue capacity"
    );

    require(
        error == "Orchestrator task queue is full",
        "Unexpected queue-full error: " + error
    );

    std::string unregister_error;

    require(
        !advanced.unregister_agent(
            executor_identity,
            unregister_error
        ),
        "Agent was unregistered while it had an active task"
    );

    require(
        unregister_error ==
            "Agent has active tasks and cannot be unregistered",
        "Unexpected active-agent unregister error: " + unregister_error
    );

    // Dos hilos solicitan el apagado concurrentemente.
    std::thread stopper_a([&advanced]() {
        advanced.stop();
    });

    std::thread stopper_b([&advanced]() {
        advanced.stop();
    });

    mcp::TaskSnapshot pending_snapshot;

    require(
        wait_for_terminal(
            advanced,
            pending_task_id,
            pending_snapshot,
            3000ms
        ),
        "Pending task was not finalized during stop"
    );

    require(
        pending_snapshot.state == mcp::TaskState::FAILED,
        "Pending task was not marked FAILED during stop"
    );

    // Permitir que la RPC activa termine para que stop() pueda unir workers.
    release_gate(*shutdown_gate);

    stopper_a.join();
    stopper_b.join();

    // stop() debe seguir siendo seguro después del apagado.
    advanced.stop();

    // No se deben aceptar nuevas tareas después de stop().
    std::string after_stop_task_id;
    require(
        !advanced.submit(
            queued_request,
            after_stop_task_id,
            error
        ),
        "Orchestrator accepted a task after stop"
    );

    std::cout << "[PASS] Concurrent and idempotent Orchestrator stop\n";
    std::cout << "[PASS] Submit rejected after Orchestrator stop\n";

    mcp::TaskSnapshot active_snapshot;

    require(
        advanced.get_task(active_task_id, active_snapshot),
        "Unable to retrieve active task after stop"
    );

    require(
        active_snapshot.state == mcp::TaskState::COMPLETED,
        "Active task did not complete cleanly during stop: " +
            active_snapshot.error
    );

    require(
        advanced.unregister_agent(executor_identity, error),
        "Agent could not be unregistered after task completion: " + error
    );

    std::cout << "[PASS] Queue capacity and active unregister protection\n";
    std::cout << "[PASS] Stop with active and pending tasks\n";

    // Test 7: AgentRpc::stop() wakes an Orchestrator worker
    // waiting for a remote RPC response.
    {
        mcp::AgentRegistry stop_registry;

        require(
            stop_registry.register_agent({
                orchestrator_identity,
                "principal-a",
                "orchestrator-test",
                true
            }, error),
            "Unable to register RPC-stop orchestrator identity: " + error
        );

        require(
            stop_registry.register_agent({
                executor_identity,
                "principal-b",
                "orchestrator-test",
                true
            }, error),
            "Unable to register RPC-stop executor identity: " + error
        );

        mcp::AgentBus stop_bus_a(stop_registry);
        mcp::AgentBus stop_bus_b(stop_registry);

        auto stop_transports = make_transport_pair();

        require(
            stop_bus_a.add_transport(stop_transports.first, error),
            "Unable to add RPC-stop transport A: " + error
        );

        require(
            stop_bus_b.add_transport(stop_transports.second, error),
            "Unable to add RPC-stop transport B: " + error
        );

        mcp::AgentRpc stop_rpc_a(
            stop_bus_a,
            orchestrator_identity,
            "principal-a"
        );

        mcp::AgentRpc stop_rpc_b(
            stop_bus_b,
            executor_identity,
            "principal-b"
        );

        auto rpc_stop_gate = std::make_shared<GateState>();

        require(
            stop_rpc_b.register_method(
                "test.gated.rpc_stop",
                make_gated_handler(rpc_stop_gate, "rpc-stop-ok"),
                error
            ),
            "Unable to register RPC-stop gate: " + error
        );

        mcp::OrchestratorConfig stop_config;
        stop_config.worker_count = 1;
        stop_config.default_timeout = 5000ms;

        mcp::Orchestrator stop_orchestrator(
            stop_registry,
            stop_rpc_a,
            stop_config
        );

        mcp::AgentDescriptor stop_executor;
        stop_executor.identity = executor_identity;
        stop_executor.capabilities = {"test.gated.rpc_stop"};

        require(
            stop_orchestrator.register_agent(stop_executor, error),
            "Unable to register RPC-stop executor: " + error
        );

        require(
            stop_orchestrator.start(error),
            "Unable to start RPC-stop Orchestrator: " + error
        );

        mcp::TaskRequest stop_request;
        stop_request.method = "test.gated.rpc_stop";
        stop_request.required_capabilities = {
            "test.gated.rpc_stop"
        };
        stop_request.timeout = 5000ms;

        std::string stop_task_id;

        require(
            stop_orchestrator.submit(
                stop_request,
                stop_task_id,
                error
            ),
            "Unable to submit RPC-stop task: " + error
        );

        const bool remote_handler_entered =
            wait_for_entries(*rpc_stop_gate, 1, 3000ms);

        if (!remote_handler_entered) {
            release_gate(*rpc_stop_gate);
        }

        require(
            remote_handler_entered,
            "Remote RPC-stop handler did not start"
        );

        // This must wake the pending AgentRpc::call() immediately.
        stop_rpc_a.stop();

        mcp::TaskSnapshot stopped_rpc_snapshot;

        const bool rpc_stop_finished = wait_for_terminal(
            stop_orchestrator,
            stop_task_id,
            stopped_rpc_snapshot,
            2000ms
        );

        // Always release the remote handler before asserting the result.
        release_gate(*rpc_stop_gate);

        require(
            rpc_stop_finished,
            "Orchestrator task did not finish after AgentRpc::stop()"
        );

        require(
            stopped_rpc_snapshot.state == mcp::TaskState::FAILED,
            "Task was not FAILED after AgentRpc::stop()"
        );

        require(
            stopped_rpc_snapshot.error == "AgentRpc stopped",
            "Unexpected error after AgentRpc::stop(): " +
                stopped_rpc_snapshot.error
        );

        // The remote handler can now return; shut down its RPC and bus.
        stop_rpc_b.stop();
        stop_orchestrator.stop();

        stop_bus_a.stop();
        stop_bus_b.stop();

        std::cout << "[PASS] AgentRpc stop wakes pending Orchestrator RPC\n";
    }

    // AgentRpc owns the AgentBus message handler.
    rpc_a.stop();
    rpc_b.stop();

    bus_a.stop();
    bus_b.stop();
}

} // namespace

int main()
{
    try {
        run_tests();
        std::cout << "\nAll orchestrator tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "\n[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
