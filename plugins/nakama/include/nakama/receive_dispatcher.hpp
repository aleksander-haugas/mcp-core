#ifndef NAKAMA_RECEIVE_DISPATCHER_HPP
#define NAKAMA_RECEIVE_DISPATCHER_HPP

#include "nakama/websocket_transport.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace nakama {

class ReceiveDispatcher {
public:
    using MessageHandler =
        std::function<void(const WebSocketMessage&)>;

    using ErrorHandler =
        std::function<void(const std::string&)>;

    explicit ReceiveDispatcher(
        std::shared_ptr<WebSocketTransport> transport,
        MessageHandler message_handler = {},
        ErrorHandler error_handler = {}
    );

    ~ReceiveDispatcher();

    ReceiveDispatcher(const ReceiveDispatcher&) = delete;
    ReceiveDispatcher& operator=(const ReceiveDispatcher&) = delete;

    ReceiveDispatcher(ReceiveDispatcher&&) = delete;
    ReceiveDispatcher& operator=(ReceiveDispatcher&&) = delete;

    bool start();

    void stop();

    bool register_pending(const std::string& cid);

    bool wait_for_response(
        const std::string& cid,
        std::chrono::milliseconds timeout,
        WebSocketMessage& response
    );

    void cancel_pending(const std::string& cid);

    std::size_t pending_count() const;

private:
    struct PendingRequest {
        std::mutex mutex;
        std::condition_variable condition;

        bool completed = false;
        WebSocketMessage response;
        std::string error;
    };

    void run();

    void dispatch_message(WebSocketMessage message);

    bool resolve_pending(
        const std::string& cid,
        const WebSocketMessage& message
    );

    void fail_all_pending(const std::string& error);

    std::shared_ptr<WebSocketTransport> transport_;

    MessageHandler message_handler_;
    ErrorHandler error_handler_;

    mutable std::mutex lifecycle_mutex_;
    std::mutex join_mutex_;
    mutable std::mutex pending_mutex_;

    std::unordered_map<
        std::string,
        std::shared_ptr<PendingRequest>
    > pending_requests_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> terminal_notified_{false};

    std::thread worker_;
};

} // namespace nakama

#endif
