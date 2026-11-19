#include "mcp/agent_bus.hpp"

#include <utility>
#include <map>
#include <mutex>
#include <exception>

namespace mcp {

struct AgentBus::SharedState {
    explicit SharedState(AgentRegistry& agent_registry)
        : registry(&agent_registry)
    {
    }

    AgentRegistry* registry;

    std::mutex lifecycle_mutex;
    std::mutex mutex;
    struct TransportEntry {
        std::shared_ptr<AgentTransport> transport;
        std::size_t active_sends = 0;
        std::size_t active_callbacks = 0;
        bool ready = false;
        bool stopping = false;
    };

    std::condition_variable sends_finished;

    std::map<std::string, TransportEntry> transports;

    MessageHandler handler;

    bool accepting = true;
};

namespace {

bool valid_transport_id(const std::string& value)
{
    if (value.empty() ||
        value.size() > agent_protocol::MAX_IDENTIFIER_SIZE) {
        return false;
    }

    for (unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7f) {
            return false;
        }
    }

    return true;
}

} // namespace

void AgentBus::finish_send(
    const std::shared_ptr<SharedState>& state,
    const std::string& transport_id
) noexcept
{
    std::lock_guard<std::mutex> lock(state->mutex);

    const auto it = state->transports.find(transport_id);

    if (it != state->transports.end() &&
        it->second.active_sends > 0) {
        --it->second.active_sends;
    }

    state->sends_finished.notify_all();
}

AgentBus::AgentBus(AgentRegistry& registry)
    : state_(std::make_shared<SharedState>(registry))
{
}

AgentBus::~AgentBus()
{
    stop();
}

bool AgentBus::add_transport(
    std::shared_ptr<AgentTransport> transport,
    std::string& error
)
{
    error.clear();

    if (!transport) {
        error = "Transport is null";
        return false;
    }

    std::string id;

    try {
        id = transport->transport_id();
    } catch (...) {
        error = "Transport ID retrieval failed";
        return false;
    }

    if (!valid_transport_id(id)) {
        error = "Invalid transport ID";
        return false;
    }

    const auto state = state_;
    std::lock_guard<std::mutex> lifecycle_lock(
        state->lifecycle_mutex
    );

    {
        std::lock_guard<std::mutex> lock(state->mutex);

        if (!state->accepting) {
            error = "AgentBus is stopped";
            return false;
        }

        if (state->transports.find(id) != state->transports.end()) {
            error = "Transport is already registered";
            return false;
        }

        SharedState::TransportEntry entry;
        entry.transport = transport;

        state->transports.emplace(id, std::move(entry));
    }

    bool started = false;

    try {
        const std::weak_ptr<SharedState> weak_state = state;

        started = transport->start(
            [weak_state, id](
                const std::string& principal,
                const std::string& message
            ) {
                AgentBus::receive(
                    weak_state,
                    id,
                    principal,
                    message
                );
            },
            error
        );
    } catch (const std::exception& exception) {
        error = exception.what();
    } catch (...) {
        error = "Transport startup threw an unknown exception";
    }

    if (!started) {
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            auto it = state->transports.find(id);

            if (it != state->transports.end()) {
                it->second.stopping = true;

                state->sends_finished.wait(
                    lock,
                    [&state, &id] {
                        const auto current = state->transports.find(id);
                        return current == state->transports.end() ||
                            (current->second.active_sends == 0 &&
                             current->second.active_callbacks == 0);
                    }
                );

                it = state->transports.find(id);
                if (it != state->transports.end()) {
                    state->transports.erase(it);
                }
            }
        }

        transport->stop();

        if (error.empty()) {
            error = "Transport startup failed";
        }

        return false;
    }

    bool published = false;

    {
        std::lock_guard<std::mutex> lock(state->mutex);

        const auto it = state->transports.find(id);

        if (it != state->transports.end() &&
            state->accepting) {
            it->second.ready = true;
            published = true;
        }
    }

    if (!published) {
        error = "AgentBus stopped during transport startup";
        transport->stop();
        return false;
    }

    return true;
}

bool AgentBus::remove_transport(
    const std::string& transport_id,
    std::string& error
)
{
    error.clear();

    if (!valid_transport_id(transport_id)) {
        error = "Invalid transport ID";
        return false;
    }

    const auto state = state_;
    std::lock_guard<std::mutex> lifecycle_lock(
        state->lifecycle_mutex
    );

    std::shared_ptr<AgentTransport> transport;

    {
        std::unique_lock<std::mutex> lock(state->mutex);

        const auto it = state->transports.find(transport_id);

        if (it == state->transports.end()) {
            error = "Transport is not registered";
            return false;
        }

        it->second.stopping = true;

        state->sends_finished.wait(
            lock,
            [&state, &transport_id] {
                const auto current =
                    state->transports.find(transport_id);

                return current == state->transports.end() ||
                    (current->second.active_sends == 0 &&
                     current->second.active_callbacks == 0);
            }
        );

        const auto current = state->transports.find(transport_id);

        if (current == state->transports.end()) {
            error = "Transport registration disappeared";
            return false;
        }

        transport = current->second.transport;
        state->transports.erase(current);
    }

    transport->stop();

    return true;
}

void AgentBus::set_message_handler(MessageHandler handler)
{
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);

    if (!state->accepting) {
        return;
    }

    state->handler = std::move(handler);
}

void AgentBus::receive(
    const std::weak_ptr<SharedState>& weak_state,
    const std::string& transport_id,
    const std::string& authenticated_principal,
    const std::string& message_json
) noexcept
{
    const auto state = weak_state.lock();

    if (!state) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);

        const auto it = state->transports.find(transport_id);

        if (!state->accepting ||
            it == state->transports.end() ||
            it->second.stopping) {
            return;
        }

        ++it->second.active_callbacks;
    }

    struct CallbackGuard {
        std::shared_ptr<SharedState> state;
        std::string transport_id;

        ~CallbackGuard()
        {
            std::lock_guard<std::mutex> lock(state->mutex);

            const auto it = state->transports.find(transport_id);

            if (it != state->transports.end() &&
                it->second.active_callbacks > 0) {
                --it->second.active_callbacks;
            }

            state->sends_finished.notify_all();
        }
    } guard{state, transport_id};

    try {
        AgentMessage message;
        std::string error;

        if (!agent_protocol::parse_message(
                message_json,
                message,
                error)) {
            return;
        }

        AgentRegistration source;

        if (!state->registry->resolve_principal(
                authenticated_principal,
                source)) {
            return;
        }

        if (!source.active ||
            source.transport_id != transport_id ||
            source.identity.agent_id != message.source.agent_id ||
            source.identity.instance_id != message.source.instance_id) {
            return;
        }

        if (message.has_target) {
            AgentRegistration destination;

            if (!state->registry->resolve(
                    message.target,
                    destination) ||
                !destination.active) {
                return;
            }
        }

        MessageHandler handler;

        {
            std::lock_guard<std::mutex> lock(state->mutex);

            if (!state->accepting) {
                return;
            }

            handler = state->handler;
        }

        if (handler) {
            try {
                handler(source, message);
            } catch (...) {
                // Las excepciones del consumidor no salen del transporte.
            }
        }
    } catch (...) {
        // No se permite que una excepción escape al worker del transporte.
    }
}

bool AgentBus::send(
    const std::string& authenticated_principal,
    const std::string& message_json,
    std::string& error
)
{
    error.clear();

    const auto state = state_;

    {
        std::lock_guard<std::mutex> lock(state->mutex);

        if (!state->accepting) {
            error = "AgentBus is stopped";
            return false;
        }
    }

    AgentMessage message;

    if (!agent_protocol::parse_message(
            message_json,
            message,
            error)) {
        return false;
    }

    AgentRegistration source;

    if (!state->registry->resolve_principal(
            authenticated_principal,
            source)) {
        error = "Authenticated principal is not registered";
        return false;
    }

    if (!source.active) {
        error = "Source agent is inactive";
        return false;
    }

    if (source.identity.agent_id != message.source.agent_id ||
        source.identity.instance_id != message.source.instance_id) {
        error = "Message source does not match authenticated identity";
        return false;
    }

    if (!message.has_target) {
        error = "Unicast send requires a target identity";
        return false;
    }

    AgentRegistration destination;

    if (!state->registry->resolve(
            message.target,
            destination)) {
        error = "Destination agent is not registered";
        return false;
    }

    if (!destination.active) {
        error = "Destination agent is inactive";
        return false;
    }

    std::shared_ptr<AgentTransport> transport;

    {
        std::lock_guard<std::mutex> lock(state->mutex);

        if (!state->accepting) {
            error = "AgentBus is stopped";
            return false;
        }

        const auto it = state->transports.find(
            destination.transport_id
        );

        if (it == state->transports.end() ||
            !it->second.ready ||
            it->second.stopping) {
            error = "Destination transport is unavailable";
            return false;
        }

        transport = it->second.transport;
        ++it->second.active_sends;
    }

    bool sent = false;

    try {
        sent = transport->send(
            destination,
            message_json,
            error
        );
    } catch (const std::exception& exception) {
        error = exception.what();
    } catch (...) {
        error = "Transport send threw an unknown exception";
    }

    finish_send(state, destination.transport_id);
    return sent;
}

void AgentBus::stop() noexcept
{
    const auto state = state_;

    if (!state) {
        return;
    }

    std::lock_guard<std::mutex> lifecycle_lock(
        state->lifecycle_mutex
    );

    std::map<std::string, SharedState::TransportEntry> transports;

    {
        std::unique_lock<std::mutex> lock(state->mutex);

        if (!state->accepting && state->transports.empty()) {
            return;
        }

        state->accepting = false;
        state->handler = nullptr;

        for (auto& entry : state->transports) {
            entry.second.stopping = true;
        }

        state->sends_finished.wait(
            lock,
            [&state] {
                for (const auto& entry : state->transports) {
                    if (entry.second.active_sends != 0 ||
                        entry.second.active_callbacks != 0) {
                        return false;
                    }
                }

                return true;
            }
        );

        transports.swap(state->transports);
    }

    for (const auto& entry : transports) {
        entry.second.transport->stop();
    }

}

} // namespace mcp
