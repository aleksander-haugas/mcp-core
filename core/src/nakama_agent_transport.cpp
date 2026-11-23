#include "mcp/nakama_agent_transport.hpp"

#include "mcp/agent_protocol.hpp"

#include <cstdint>
#include <exception>
#include <utility>

namespace mcp {

namespace {

constexpr const char* NAKAMA_PLUGIN_ID = "nakama";
constexpr const char* NAKAMA_TRANSPORT_ID = "nakama";

} // namespace

NakamaAgentTransport::NakamaAgentTransport(
    PluginManager& plugin_manager
)
    : plugin_manager_(plugin_manager)
{
}

NakamaAgentTransport::~NakamaAgentTransport()
{
    stop();
}

std::string NakamaAgentTransport::transport_id() const
{
    return NAKAMA_TRANSPORT_ID;
}

bool NakamaAgentTransport::start(
    MessageHandler handler,
    std::string& error
)
{
    error.clear();
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

    if (!handler) {
        error = "Nakama transport requires a message handler";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (started_) {
            error = "Nakama transport is already started";
            return false;
        }

        handler_ = std::move(handler);
        accepting_ = true;
        started_ = true;
    }

    try {
        /*
         * PluginManager exposes one global authenticated handler.
         * This transport must be its exclusive owner while running.
         */
        plugin_manager_.set_authenticated_message_handler(
            [this](const AuthenticatedPluginMessage& message) {
                receive(message);
            }
        );
    } catch (const std::exception& exception) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            accepting_ = false;
            started_ = false;
            handler_ = nullptr;
        }

        error = std::string(
            "Unable to install Nakama message handler: "
        ) + exception.what();

        return false;
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            accepting_ = false;
            started_ = false;
            handler_ = nullptr;
        }

        error = "Unable to install Nakama message handler";
        return false;
    }

    return true;
}

void NakamaAgentTransport::receive(
    const AuthenticatedPluginMessage& message
) noexcept
{
    MessageHandler handler;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!accepting_ ||
            message.plugin_id != NAKAMA_PLUGIN_ID ||
            message.transport_id != NAKAMA_TRANSPORT_ID ||
            message.authenticated_principal.empty() ||
            message.data.size() > agent_protocol::MAX_MESSAGE_SIZE) {
            return;
        }

        try {
            handler = handler_;
        } catch (...) {
            return;
        }

        if (!handler) {
            return;
        }

        ++active_callbacks_;
    }

    struct CallbackGuard {
        NakamaAgentTransport* owner;

        ~CallbackGuard()
        {
            std::lock_guard<std::mutex> lock(owner->mutex_);

            if (owner->active_callbacks_ > 0) {
                --owner->active_callbacks_;
            }

            if (owner->active_callbacks_ == 0) {
                owner->callbacks_drained_.notify_all();
            }
        }
    } guard{this};

    try {
        const std::string message_json(
            message.data.begin(),
            message.data.end()
        );

        handler(
            message.authenticated_principal,
            message_json
        );
    } catch (...) {
        // Exceptions must never escape into PluginManager's worker.
    }
}

bool NakamaAgentTransport::send(
    const AgentRegistration& destination,
    const std::string& message_json,
    std::string& error
)
{
    error.clear();

    if (!destination.active) {
        error = "Nakama destination agent is not active";
        return false;
    }

    if (destination.transport_id != NAKAMA_TRANSPORT_ID) {
        error = "Destination does not use the Nakama transport";
        return false;
    }

    if (destination.principal_id.empty()) {
        error = "Destination principal is empty";
        return false;
    }

    if (message_json.empty() ||
        message_json.size() > agent_protocol::MAX_MESSAGE_SIZE) {
        error = "Invalid Nakama message size";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!started_ || !accepting_) {
            error = "Nakama transport is not running";
            return false;
        }

        ++active_sends_;
    }

    bool sent = false;

    try {
        sent = plugin_manager_.send_plugin_message(
            NAKAMA_PLUGIN_ID,
            reinterpret_cast<const std::uint8_t*>(
                message_json.data()
            ),
            message_json.size(),
            error
        );
    } catch (const std::exception& exception) {
        error = std::string(
            "Nakama message enqueue failed: "
        ) + exception.what();
    } catch (...) {
        error = "Nakama message enqueue failed";
    }

    finish_send();
    return sent;
}

void NakamaAgentTransport::finish_send() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (active_sends_ > 0) {
        --active_sends_;
    }

    if (active_sends_ == 0) {
        callbacks_drained_.notify_all();
    }
}

void NakamaAgentTransport::stop() noexcept
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!started_) {
            return;
        }

        accepting_ = false;
        started_ = false;
        handler_ = nullptr;
    }

    /*
     * This call drains PluginManager's active authenticated callbacks.
     * It must run outside the authenticated callback itself.
     */
    try {
        plugin_manager_.clear_authenticated_message_handler();
    } catch (...) {
        // stop() is noexcept.
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);

        callbacks_drained_.wait(
            lock,
            [this] {
                return active_callbacks_ == 0 &&
                   active_sends_ == 0;
            }
        );
    }
}

} // namespace mcp
