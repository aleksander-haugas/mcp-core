#include "nakama/receive_dispatcher.hpp"

#include <json-c/json.h>

#include <cstring>
#include <exception>
#include <utility>

namespace nakama {

namespace {

thread_local const ReceiveDispatcher* active_dispatcher = nullptr;

std::string state_error(
    TransportState state,
    const std::string& transport_error)
{
    if (!transport_error.empty()) {
        return transport_error;
    }

    switch (state) {
    case TransportState::STOPPED:
        return "WebSocket transport stopped";
    case TransportState::ERROR:
        return "WebSocket transport entered ERROR state";
    case TransportState::CLOSING:
        return "WebSocket transport is closing";
    case TransportState::CONNECTING:
        return "WebSocket transport is connecting";
    case TransportState::CONNECTED:
        return {};
    }

    return "Unknown WebSocket transport state";
}

bool extract_pong_cid(
    const WebSocketMessage& message,
    std::string& cid)
{
    if (message.type != WebSocketMessageType::TEXT ||
        message.payload.empty() ||
        message.payload.size() >
            static_cast<std::size_t>(1024 * 1024)) {
        return false;
    }

    json_tokener* tokener = json_tokener_new();

    if (tokener == nullptr) {
        return false;
    }

    json_tokener_set_flags(
        tokener,
        JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8
    );

    json_object* root = json_tokener_parse_ex(
        tokener,
        message.payload.data(),
        static_cast<int>(message.payload.size())
    );

    const bool parsed =
        json_tokener_get_error(tokener) == json_tokener_success &&
        root != nullptr;

    const std::size_t parse_end =
        json_tokener_get_parse_end(tokener);

    json_tokener_free(tokener);

    if (!parsed) {
        if (root != nullptr) {
            json_object_put(root);
        }
        return false;
    }

    // Reject non-whitespace trailing data.
    for (std::size_t i = parse_end;
         i < message.payload.size(); ++i) {
        const unsigned char c =
            static_cast<unsigned char>(message.payload[i]);

        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            json_object_put(root);
            return false;
        }
    }

    json_object* cid_object = nullptr;
    json_object* pong_object = nullptr;

    const bool valid =
        json_object_is_type(root, json_type_object) &&
        json_object_object_get_ex(root, "cid", &cid_object) &&
        cid_object != nullptr &&
        json_object_is_type(cid_object, json_type_string) &&
        json_object_object_get_ex(root, "pong", &pong_object) &&
        pong_object != nullptr &&
        json_object_is_type(pong_object, json_type_object);

    if (valid) {
        const char* value = json_object_get_string(cid_object);
        const int length = json_object_get_string_len(cid_object);

        if (value != nullptr && length > 0 &&
            std::strlen(value) == static_cast<std::size_t>(length)) {
            cid.assign(value, static_cast<std::size_t>(length));
        }
    }

    json_object_put(root);
    return valid && !cid.empty();
}
} // namespace

ReceiveDispatcher::ReceiveDispatcher(
    std::shared_ptr<WebSocketTransport> transport,
    MessageHandler message_handler,
    ErrorHandler error_handler)
    : transport_(std::move(transport)),
      message_handler_(std::move(message_handler)),
      error_handler_(std::move(error_handler))
{
}

ReceiveDispatcher::~ReceiveDispatcher()
{
    stop();
}

bool ReceiveDispatcher::start()
{
    std::lock_guard<std::mutex> join_lock(join_mutex_);
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

    if (!transport_ ||
        transport_->state() != TransportState::CONNECTED ||
        stop_requested_.load() ||
        terminal_notified_.load()) {
        return false;
    }

    if (worker_.joinable()) {
        return true;
    }

    try {
        worker_ = std::thread(&ReceiveDispatcher::run, this);
    } catch (const std::exception&) {
        return false;
    }

    return true;
}

void ReceiveDispatcher::stop()
{
    // A callback running on this worker must never wait for its own join.
    if (active_dispatcher == this) {
        {
            std::lock_guard<std::mutex> lifecycle_lock(
                lifecycle_mutex_
            );
            stop_requested_.store(true);
        }

        fail_all_pending("Receive dispatcher stopped");
        return;
    }

    std::lock_guard<std::mutex> join_lock(join_mutex_);

    {
        std::lock_guard<std::mutex> lifecycle_lock(
            lifecycle_mutex_
        );
        stop_requested_.store(true);
    }

    fail_all_pending("Receive dispatcher stopped");

    if (worker_.joinable()) {
        worker_.join();
    }
}

bool ReceiveDispatcher::register_pending(const std::string& cid)
{
    if (cid.empty() ||
        stop_requested_.load() ||
        terminal_notified_.load()) {
        return false;
    }

    auto request = std::make_shared<PendingRequest>();

    std::lock_guard<std::mutex> lock(pending_mutex_);

    if (stop_requested_.load() ||
        terminal_notified_.load() ||
        pending_requests_.find(cid) != pending_requests_.end()) {
        return false;
    }

    pending_requests_.emplace(cid, std::move(request));
    return true;
}

bool ReceiveDispatcher::wait_for_response(
    const std::string& cid,
    std::chrono::milliseconds timeout,
    WebSocketMessage& response)
{
    std::shared_ptr<PendingRequest> request;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);

        const auto it = pending_requests_.find(cid);

        if (it == pending_requests_.end()) {
            return false;
        }

        request = it->second;
    }

    bool success = false;

    {
        std::unique_lock<std::mutex> lock(request->mutex);

        const bool completed = request->condition.wait_for(
            lock,
            timeout,
            [&request]() {
                return request->completed;
            }
        );

        success = completed && request->error.empty();

        if (success) {
            response = std::move(request->response);
        }
    }

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);

        const auto it = pending_requests_.find(cid);

        if (it != pending_requests_.end() &&
            it->second == request) {
            pending_requests_.erase(it);
        }
    }

    return success;
}

void ReceiveDispatcher::cancel_pending(const std::string& cid)
{
    std::shared_ptr<PendingRequest> request;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);

        const auto it = pending_requests_.find(cid);

        if (it == pending_requests_.end()) {
            return;
        }

        request = it->second;
        pending_requests_.erase(it);
    }

    {
        std::lock_guard<std::mutex> lock(request->mutex);
        request->error = "Request cancelled";
        request->completed = true;
    }

    request->condition.notify_all();
}

std::size_t ReceiveDispatcher::pending_count() const
{
    std::lock_guard<std::mutex> lock(pending_mutex_);
    return pending_requests_.size();
}

void ReceiveDispatcher::run()
{
    const ReceiveDispatcher* previous_dispatcher =
        active_dispatcher;

    active_dispatcher = this;

    struct ActiveDispatcherRestore {
        const ReceiveDispatcher*& slot;
        const ReceiveDispatcher* previous;

        ~ActiveDispatcherRestore()
        {
            slot = previous;
        }
    } restore{active_dispatcher, previous_dispatcher};

    const auto notify_terminal = [this](
        const std::string& error
    ) noexcept {
        const bool first_terminal =
            !terminal_notified_.exchange(true);

        try {
            fail_all_pending(error);
        } catch (...) {
            // Best effort: never let failure handling escape the worker.
        }

        if (first_terminal && error_handler_) {
            try {
                error_handler_(error);
            } catch (...) {
                // Callback failures must not escape the worker.
            }
        }
    };

    try {
        while (!stop_requested_.load()) {
            WebSocketMessage message;

            if (transport_->try_receive(message)) {
                dispatch_message(std::move(message));
                continue;
            }

            const TransportState state = transport_->state();

            if (state == TransportState::ERROR ||
                state == TransportState::STOPPED ||
                state == TransportState::CLOSING) {
                const std::string error = state_error(
                    state,
                    transport_->last_error()
                );

                notify_terminal(error);
                return;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(10)
            );
        }
    } catch (const std::exception&) {
        notify_terminal(
            "Receive dispatcher worker exception"
        );
    } catch (...) {
        notify_terminal(
            "Unknown receive dispatcher worker exception"
        );
    }
}
void ReceiveDispatcher::dispatch_message(WebSocketMessage message)
{
    std::string cid;

    if (extract_pong_cid(message, cid) &&
        resolve_pending(cid, message)) {
        return;
    }

    if (message_handler_) {
        try {
            message_handler_(message);
        } catch (const std::exception&) {
            // Isolate callback failures from the receive loop.
        } catch (...) {
            // Isolate unknown callback failures as well.
        }
    }
}

bool ReceiveDispatcher::resolve_pending(
    const std::string& cid,
    const WebSocketMessage& message)
{
    std::shared_ptr<PendingRequest> request;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);

        const auto it = pending_requests_.find(cid);

        if (it == pending_requests_.end()) {
            return false;
        }

        request = it->second;
    }

    {
        std::lock_guard<std::mutex> lock(request->mutex);

        if (request->completed) {
            return false;
        }

        request->response = message;
        request->completed = true;
    }

    request->condition.notify_all();
    return true;
}

void ReceiveDispatcher::fail_all_pending(const std::string& error)
{
    std::unordered_map<
        std::string,
        std::shared_ptr<PendingRequest>
    > requests;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        requests.swap(pending_requests_);
    }

    for (auto& entry : requests) {
        const auto& request = entry.second;

        {
            std::lock_guard<std::mutex> lock(request->mutex);

            if (!request->completed) {
                request->error = error;
                request->completed = true;
            }
        }

        request->condition.notify_all();
    }
}

} // namespace nakama
