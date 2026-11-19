#include "mcp/agent_rpc.hpp"

#include <json-c/json.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace mcp {
namespace {

bool same_identity(
    const AgentIdentity& left,
    const AgentIdentity& right
)
{
    return left.agent_id == right.agent_id &&
           left.instance_id == right.instance_id;
}

std::string make_timestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};

    if (gmtime_r(&now, &utc) == nullptr) {
        return {};
    }

    char buffer[32]{};

    if (std::strftime(
            buffer,
            sizeof(buffer),
            "%Y-%m-%dT%H:%M:%SZ",
            &utc) == 0) {
        return {};
    }

    return buffer;
}

std::string make_message_id()
{
    static std::atomic<unsigned long long> sequence{0};

    const auto ticks =
        std::chrono::steady_clock::now().time_since_epoch().count();

    const auto number =
        sequence.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream output;
    output << "rpc-" << ticks << "-" << number;
    return output.str();
}

bool make_error_payload(
    const std::string& code,
    const std::string& message,
    std::string& output
)
{
    json_object* object = json_object_new_object();

    if (object == nullptr) {
        return false;
    }

    json_object* code_value = json_object_new_string(code.c_str());
    json_object* message_value =
        json_object_new_string(message.c_str());

    if (code_value == nullptr || message_value == nullptr) {
        if (code_value != nullptr) {
            json_object_put(code_value);
        }

        if (message_value != nullptr) {
            json_object_put(message_value);
        }

        json_object_put(object);
        return false;
    }

    json_object_object_add(object, "code", code_value);
    json_object_object_add(object, "message", message_value);

    const char* serialized = json_object_to_json_string_ext(
        object,
        JSON_C_TO_STRING_PLAIN
    );

    if (serialized == nullptr) {
        json_object_put(object);
        return false;
    }

    output = serialized;
    json_object_put(object);
    return true;
}

bool read_remote_error(
    const std::string& payload,
    std::string& error
)
{
    json_tokener* tokener = json_tokener_new();

    if (tokener == nullptr) {
        error = "Remote agent returned an error";
        return false;
    }

    json_object* object = json_tokener_parse_ex(
        tokener,
        payload.c_str(),
        static_cast<int>(payload.size())
    );

    const bool valid =
        json_tokener_get_error(tokener) == json_tokener_success &&
        object != nullptr &&
        json_object_get_type(object) == json_type_object;

    if (!valid) {
        if (object != nullptr) {
            json_object_put(object);
        }

        json_tokener_free(tokener);
        error = "Remote agent returned an invalid error payload";
        return false;
    }

    json_object* code_value = nullptr;
    json_object* message_value = nullptr;

    const bool has_code = json_object_object_get_ex(
        object, "code", &code_value
    );

    const bool has_message = json_object_object_get_ex(
        object, "message", &message_value
    );

    if (has_code &&
        has_message &&
        json_object_get_type(code_value) == json_type_string &&
        json_object_get_type(message_value) == json_type_string) {
        error =
            std::string(json_object_get_string(code_value)) +
            ": " +
            json_object_get_string(message_value);
    } else {
        error = "Remote agent returned an error";
    }

    json_object_put(object);
    json_tokener_free(tokener);
    return true;
}

} // namespace

struct AgentRpc::SharedState {
    struct PendingCall {
        AgentIdentity expected_source;
        bool completed = false;
        AgentMessageType response_type = AgentMessageType::ERROR;
        std::string payload_json;
        std::string error;
        std::condition_variable condition;
    };

    AgentBus* bus = nullptr;
    AgentIdentity local_identity;
    std::string principal;

    std::mutex mutex;
    bool accepting = true;

    std::map<std::string, std::shared_ptr<PendingCall>> pending;
    std::map<std::string, MethodHandler> methods;
};

AgentRpc::AgentRpc(
    AgentBus& bus,
    AgentIdentity local_identity,
    std::string authenticated_principal
)
    : state_(std::make_shared<SharedState>())
{
    state_->bus = &bus;
    state_->local_identity = std::move(local_identity);
    state_->principal = std::move(authenticated_principal);

    const std::weak_ptr<SharedState> weak_state = state_;

    bus.set_message_handler(
        [weak_state](
            const AgentRegistration& source,
            const AgentMessage& message
        ) {
            const auto state = weak_state.lock();

            if (state) {
                AgentRpc::receive(state, source, message);
            }
        }
    );
}

AgentRpc::~AgentRpc()
{
    stop();
}

bool AgentRpc::register_method(
    const std::string& method,
    MethodHandler handler,
    std::string& error
)
{
    error.clear();

    if (method.empty() ||
        method.size() > agent_protocol::MAX_METHOD_SIZE ||
        method.find('.') == std::string::npos ||
        !handler) {
        error = "Invalid RPC method or empty handler";
        return false;
    }

    for (const unsigned char character : method) {
        if (character <= 0x20 || character >= 0x7f) {
            error = "RPC method contains an invalid character";
            return false;
        }
    }

    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);

    if (!state->accepting) {
        error = "AgentRpc is stopped";
        return false;
    }

    const auto inserted = state->methods.emplace(
        method,
        std::move(handler)
    );

    if (!inserted.second) {
        error = "RPC method is already registered";
        return false;
    }

    return true;
}

bool AgentRpc::call(
    const AgentIdentity& target,
    const std::string& method,
    const std::string& payload_json,
    std::string& result_json,
    std::string& error,
    unsigned int timeout_ms
)
{
    result_json.clear();
    error.clear();

    const auto state = state_;

    if (target.agent_id.empty() || target.instance_id.empty()) {
        error = "Invalid RPC target identity";
        return false;
    }

    if (timeout_ms == 0) {
        error = "RPC timeout must be greater than zero";
        return false;
    }

    AgentMessage request;
    request.type = AgentMessageType::REQUEST;
    request.message_id = make_message_id();
    request.source = state->local_identity;
    request.target = target;
    request.has_target = true;
    request.method = method;
    request.has_method = true;
    request.payload_json = payload_json;
    request.timestamp = make_timestamp();

    if (request.timestamp.empty()) {
        error = "Unable to generate UTC timestamp";
        return false;
    }

    std::string serialized;
    if (!agent_protocol::serialize_message(
            request, serialized, error)) {
        return false;
    }

    auto pending = std::make_shared<SharedState::PendingCall>();
    pending->expected_source = target;

    {
        std::lock_guard<std::mutex> lock(state->mutex);

        if (!state->accepting) {
            error = "AgentRpc is stopped";
            return false;
        }

        state->pending.emplace(request.message_id, pending);
    }

    std::string send_error;

    if (!state->bus->send(
            state->principal,
            serialized,
            send_error)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->pending.erase(request.message_id);

        error = send_error.empty()
            ? "Unable to send RPC request"
            : send_error;

        return false;
    }

    std::unique_lock<std::mutex> lock(state->mutex);

    const bool completed = pending->condition.wait_for(
        lock,
        std::chrono::milliseconds(timeout_ms),
        [&pending]() {
            return pending->completed;
        }
    );

    const auto it = state->pending.find(request.message_id);

    if (it != state->pending.end() && it->second == pending) {
        state->pending.erase(it);
    }

    if (!completed) {
        error = "RPC request timed out";
        return false;
    }

    if (!pending->error.empty()) {
        error = pending->error;
        return false;
    }

    if (pending->response_type == AgentMessageType::ERROR) {
        read_remote_error(pending->payload_json, error);
        return false;
    }

    result_json = pending->payload_json;
    return true;
}

void AgentRpc::receive(
    const std::shared_ptr<SharedState>& state,
    const AgentRegistration& source,
    const AgentMessage& message
) noexcept
{
    try {
        if (message.type == AgentMessageType::REQUEST) {
            handle_request(state, source, message);
            return;
        }

        if (message.type != AgentMessageType::RESPONSE &&
            message.type != AgentMessageType::ERROR) {
            return;
        }

        if (!message.has_correlation_id ||
            !message.has_target ||
            !same_identity(message.target, state->local_identity)) {
            return;
        }

        std::lock_guard<std::mutex> lock(state->mutex);

        if (!state->accepting) {
            return;
        }

        const auto it = state->pending.find(message.correlation_id);

        if (it == state->pending.end()) {
            return;
        }

        const auto& pending = it->second;

        if (!same_identity(source.identity, pending->expected_source) ||
            pending->completed) {
            return;
        }

        pending->response_type = message.type;
        pending->payload_json = message.payload_json;
        pending->completed = true;
        pending->condition.notify_all();
    } catch (...) {
        // Las excepciones no deben propagarse al worker de transporte.
    }
}

void AgentRpc::handle_request(
    const std::shared_ptr<SharedState>& state,
    const AgentRegistration& source,
    const AgentMessage& message
) noexcept
{
    try {
        if (!message.has_target ||
            !same_identity(message.target, state->local_identity) ||
            !message.has_method) {
            return;
        }

        MethodHandler handler;

        {
            std::lock_guard<std::mutex> lock(state->mutex);

            if (!state->accepting) {
                return;
            }

            const auto it = state->methods.find(message.method);

            if (it != state->methods.end()) {
                handler = it->second;
            }
        }

        AgentMessage reply;
        reply.type = AgentMessageType::RESPONSE;
        reply.message_id = make_message_id();
        reply.correlation_id = message.message_id;
        reply.has_correlation_id = true;
        reply.source = state->local_identity;
        reply.target = source.identity;
        reply.has_target = true;
        reply.timestamp = make_timestamp();

        std::string handler_error;

        if (!handler) {
            reply.type = AgentMessageType::ERROR;

            if (!make_error_payload(
                    "METHOD_NOT_FOUND",
                    "RPC method is not registered",
                    reply.payload_json)) {
                return;
            }
        } else {
            std::string result_json = "{}";

            bool succeeded = false;

            try {
                succeeded = handler(
                    source.identity,
                    message.payload_json,
                    result_json,
                    handler_error
                );
            } catch (...) {
                handler_error = "RPC method handler threw an exception";
            }

            if (succeeded) {
                reply.payload_json = std::move(result_json);
            } else {
                reply.type = AgentMessageType::ERROR;

                if (handler_error.empty()) {
                    handler_error = "RPC method execution failed";
                }

                if (!make_error_payload(
                        "METHOD_EXECUTION_FAILED",
                        handler_error,
                        reply.payload_json)) {
                    return;
                }
            }
        }

        if (reply.timestamp.empty()) {
            return;
        }

        std::string serialized;
        std::string serialization_error;

        if (!agent_protocol::serialize_message(
                reply,
                serialized,
                serialization_error)) {
            return;
        }

        std::string send_error;
        state->bus->send(
            state->principal,
            serialized,
            send_error
        );
    } catch (...) {
        // No se propagan errores al hilo de recepción.
    }
}

void AgentRpc::stop() noexcept
{
    const auto state = state_;

    if (!state) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);

        if (!state->accepting) {
            return;
        }

        state->accepting = false;
        state->methods.clear();

        for (auto& entry : state->pending) {
            entry.second->error = "AgentRpc stopped";
            entry.second->completed = true;
            entry.second->condition.notify_all();
        }

        state->pending.clear();
    }

    if (state->bus != nullptr) {
        state->bus->set_message_handler(nullptr);
    }
}

} // namespace mcp
