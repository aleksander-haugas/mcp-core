#include "mock_websocket_server.hpp"

#include <openssl/evp.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <limits>
#include <string_view>
#include <cstdio>

namespace {

constexpr int POLL_INTERVAL_MS = 100;
constexpr std::size_t MAX_HTTP_HEADER = 16384;
constexpr std::size_t MAX_FRAME_SIZE = 1024 * 1024;

bool send_all(int fd, const void* data, std::size_t length)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t offset = 0;

    while (offset < length) {
        const ssize_t sent = send(
            fd,
            bytes + offset,
            length - offset,
            0
        );

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (sent == 0) {
            return false;
        }

        offset += static_cast<std::size_t>(sent);
    }

    return true;
}

bool receive_exact(int fd, void* data, std::size_t length)
{
    auto* bytes = static_cast<unsigned char*>(data);
    std::size_t offset = 0;

    while (offset < length) {
        const ssize_t received = recv(
            fd,
            bytes + offset,
            length - offset,
            0
        );

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (received == 0) {
            return false;
        }

        offset += static_cast<std::size_t>(received);
    }

    return true;
}

std::string lowercase(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    return value;
}

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");

    if (first == std::string::npos) {
        return {};
    }

    const auto last = value.find_last_not_of(" \t\r\n");

    return value.substr(first, last - first + 1);
}

std::string find_header(
    const std::string& request,
    const std::string& name)
{
    const std::string wanted = lowercase(name);
    std::size_t position = request.find("\r\n");

    while (position != std::string::npos) {
        position += 2;

        const std::size_t end = request.find("\r\n", position);

        if (end == std::string::npos || end == position) {
            break;
        }

        const std::string line =
            request.substr(position, end - position);

        const std::size_t colon = line.find(':');

        if (colon != std::string::npos) {
            const std::string key =
                lowercase(trim(line.substr(0, colon)));

            if (key == wanted) {
                return trim(line.substr(colon + 1));
            }
        }

        position = end;
    }

    return {};
}

std::string websocket_accept(const std::string& key)
{
    constexpr std::string_view GUID =
        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    const std::string input = key + std::string(GUID);

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0;

    if (EVP_Digest(
        input.data(),
        input.size(),
        digest.data(),
        &digest_length,
        EVP_sha1(),
        nullptr
    ) != 1) {
        return {};
    }

    std::string encoded(4 * ((digest_length + 2) / 3), '\0');

    const int encoded_length = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        digest.data(),
        static_cast<int>(digest_length)
    );

    if (encoded_length <= 0) {
        return {};
    }

    encoded.resize(static_cast<std::size_t>(encoded_length));
    return encoded;
}

bool make_close_payload(
    std::uint16_t code,
    const std::string& reason,
    std::string& payload)
{
    if (reason.size() > 123) {
        return false;
    }

    payload.clear();
    payload.push_back(static_cast<char>((code >> 8) & 0xff));
    payload.push_back(static_cast<char>(code & 0xff));
    payload += reason;

    return true;
}

} // namespace

MockWebSocketServer::MockWebSocketServer() = default;

MockWebSocketServer::~MockWebSocketServer()
{
    stop();
}

bool MockWebSocketServer::start()
{
    if (running_.load() || worker_.joinable()) {
        return false;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return false;
    }

    int reuse = 1;

    if (setsockopt(
        fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse)
    ) < 0) {
        close(fd);
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (bind(
        fd,
        reinterpret_cast<sockaddr*>(&address),
        sizeof(address)
    ) < 0) {
        close(fd);
        return false;
    }

    if (listen(fd, 1) < 0) {
        close(fd);
        return false;
    }

    socklen_t address_length = sizeof(address);

    if (getsockname(
        fd,
        reinterpret_cast<sockaddr*>(&address),
        &address_length
    ) < 0) {
        close(fd);
        return false;
    }

    listen_fd_ = fd;
    port_ = ntohs(address.sin_port);

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        incoming_.clear();
        pongs_.clear();
        closes_.clear();
    }

    running_.store(true);

    try {
        worker_ = std::thread(&MockWebSocketServer::run, this);
    } catch (...) {
        running_.store(false);
        close(listen_fd_);
        listen_fd_ = -1;
        port_ = 0;
        return false;
    }

    return true;
}

void MockWebSocketServer::stop()
{
    running_.store(false);

    const int client = client_fd_.load();

    if (client >= 0) {
        shutdown(client, SHUT_RDWR);
    }

    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    client_fd_.store(-1);
    connected_.store(false);
    port_ = 0;

    queue_condition_.notify_all();
}

std::uint16_t MockWebSocketServer::port() const
{
    return port_;
}

bool MockWebSocketServer::connected() const
{
    return connected_.load();
}

bool MockWebSocketServer::send_text(const std::string& message)
{
    return send_data_frame(client_fd_.load(), message, false);
}

bool MockWebSocketServer::send_binary(const std::string& message)
{
    return send_data_frame(client_fd_.load(), message, true);
}

bool MockWebSocketServer::send_data_frame(
    int client_fd,
    const std::string& payload,
    bool binary)
{

    return send_server_frame(
        client_fd,
        binary ? 0x2 : 0x1,
        true,
        payload
    );
}

bool MockWebSocketServer::send_fragmented_text(
    const std::string& message,
    std::size_t fragment_size)
{
    return send_fragmented_message(
        client_fd_.load(),
        message,
        0x1,
        fragment_size,
        nullptr
    );
}

bool MockWebSocketServer::send_fragmented_binary(
    const std::string& message,
    std::size_t fragment_size)
{
    return send_fragmented_message(
        client_fd_.load(),
        message,
        0x2,
        fragment_size,
        nullptr
    );
}

bool MockWebSocketServer::send_fragmented_text_with_ping(
    const std::string& message,
    const std::string& ping_payload,
    std::size_t fragment_size)
{
    return send_fragmented_message(
        client_fd_.load(),
        message,
        0x1,
        fragment_size,
        &ping_payload
    );
}

bool MockWebSocketServer::send_fragmented_message(
    int client_fd,
    const std::string& message,
    unsigned char opcode,
    std::size_t fragment_size,
    const std::string* interleaved_ping)
{
    if (client_fd < 0 ||
        message.size() > MAX_FRAME_SIZE ||
        !connected_.load()) {
        return false;
    }

    if (fragment_size == 0) {
        fragment_size = std::max<std::size_t>(1, message.size() / 2);
    }

    fragment_size = std::min(fragment_size, message.size());

    const std::size_t first_size = fragment_size;
    const std::string first = message.substr(0, first_size);
    const std::string second = message.substr(first_size);

    std::lock_guard<std::mutex> lock(send_mutex_);

    if (client_fd_.load() != client_fd) {
        return false;
    }

    if (!send_server_frame_locked(
        client_fd,
        opcode,
        false,
        first
    )) {
        return false;
    }

    if (interleaved_ping != nullptr) {
        if (interleaved_ping->size() > 125 ||
            !send_server_frame_locked(
                client_fd,
                0x9,
                true,
                *interleaved_ping
            )) {
            return false;
        }
    }

    return send_server_frame_locked(
        client_fd,
        0x0,
        true,
        second
    );
}

bool MockWebSocketServer::send_ping(const std::string& payload)
{
    if (payload.size() > 125) {
        return false;
    }

    const auto timestamp =
    std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();

    const bool sent = send_server_frame(
        client_fd_.load(),
                                        0x9,
                                        true,
                                        payload
    );

    std::fprintf(
        stderr,
        "[mock] PING send: timestamp=%lld sent=%d payload='%s'\n",
        static_cast<long long>(timestamp),
                 sent ? 1 : 0,
                 payload.c_str()
    );

    return sent;
}

bool MockWebSocketServer::send_close(
    std::uint16_t code,
    const std::string& reason)
{
    std::string payload;

    if (!make_close_payload(code, reason, payload)) {
        return false;
    }

    return send_server_frame(
        client_fd_.load(),
        0x8,
        true,
        payload
    );
}

bool MockWebSocketServer::send_server_frame(
    int client_fd,
    unsigned char opcode,
    bool final_frame,
    const std::string& payload)
{
    if (client_fd < 0 ||
        payload.size() > MAX_FRAME_SIZE ||
        !connected_.load()) {
        return false;
    }

    if (opcode >= 0x8 &&
        (!final_frame || payload.size() > 125)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(send_mutex_);

    if (client_fd_.load() != client_fd) {
        return false;
    }

    return send_server_frame_locked(
        client_fd,
        opcode,
        final_frame,
        payload
    );
}

bool MockWebSocketServer::send_server_frame_locked(
    int client_fd,
    unsigned char opcode,
    bool final_frame,
    const std::string& payload)
{
    if (client_fd < 0 ||
        client_fd_.load() != client_fd ||
        payload.size() > MAX_FRAME_SIZE ||
        !connected_.load()) {
        return false;
    }

    if (opcode >= 0x8 &&
        (!final_frame || payload.size() > 125)) {
        return false;
    }

    std::array<unsigned char, 10> header{};
    std::size_t header_length = 2;

    header[0] = static_cast<unsigned char>(
        (final_frame ? 0x80 : 0x00) | (opcode & 0x0f)
    );

    if (payload.size() <= 125) {
        header[1] = static_cast<unsigned char>(payload.size());
    } else if (payload.size() <= 65535) {
        header[1] = 126;
        header[2] = static_cast<unsigned char>(
            (payload.size() >> 8) & 0xff
        );
        header[3] = static_cast<unsigned char>(
            payload.size() & 0xff
        );
        header_length = 4;
    } else {
        header[1] = 127;

        const std::uint64_t length = payload.size();

        for (int i = 0; i < 8; ++i) {
            header[2 + i] = static_cast<unsigned char>(
                (length >> (56 - i * 8)) & 0xff
            );
        }

        header_length = 10;
    }

    return send_all(client_fd, header.data(), header_length) &&
        send_all(client_fd, payload.data(), payload.size());
}

bool MockWebSocketServer::wait_for_message(
    std::string& message,
    bool& binary,
    int timeout_ms)
{
    std::unique_lock<std::mutex> lock(queue_mutex_);

    const bool available = queue_condition_.wait_for(
        lock,
        std::chrono::milliseconds(timeout_ms),
        [this] {
            return !incoming_.empty() || !running_.load();
        }
    );

    if (!available || incoming_.empty()) {
        return false;
    }

    message = std::move(incoming_.front().payload);
    binary = incoming_.front().binary;
    incoming_.pop_front();

    return true;
}

bool MockWebSocketServer::wait_for_control_payload(
    std::deque<std::string>& queue,
    std::string& payload,
    int timeout_ms)
{
    std::unique_lock<std::mutex> lock(queue_mutex_);

    const bool available = queue_condition_.wait_for(
        lock,
        std::chrono::milliseconds(timeout_ms),
        [&queue, this] {
            return !queue.empty() || !running_.load();
        }
    );

    if (!available || queue.empty()) {
        return false;
    }

    payload = std::move(queue.front());
    queue.pop_front();

    return true;
}

bool MockWebSocketServer::wait_for_pong(
    std::string& payload,
    int timeout_ms)
{
    return wait_for_control_payload(pongs_, payload, timeout_ms);
}

bool MockWebSocketServer::wait_for_close(
    std::string& payload,
    int timeout_ms)
{
    return wait_for_control_payload(closes_, payload, timeout_ms);
}

bool MockWebSocketServer::perform_handshake(int client_fd)
{
    std::string request;
    request.reserve(2048);

    std::array<char, 1024> buffer{};

    while (request.find("\r\n\r\n") == std::string::npos) {
        if (request.size() >= MAX_HTTP_HEADER) {
            return false;
        }

        const ssize_t received = recv(
            client_fd,
            buffer.data(),
            buffer.size(),
            0
        );

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (received == 0) {
            return false;
        }

        request.append(
            buffer.data(),
            static_cast<std::size_t>(received)
        );
    }

    if (request.rfind("GET ", 0) != 0) {
        return false;
    }

    const std::string upgrade =
        lowercase(find_header(request, "Upgrade"));

    const std::string connection =
        lowercase(find_header(request, "Connection"));

    const std::string key =
        find_header(request, "Sec-WebSocket-Key");

    const std::string version =
        find_header(request, "Sec-WebSocket-Version");

    if (upgrade != "websocket" ||
        connection.find("upgrade") == std::string::npos ||
        key.empty() ||
        version != "13") {
        return false;
    }

    const std::string accept = websocket_accept(key);

    if (accept.empty()) {
        return false;
    }

    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n"
        "\r\n";

    return send_all(
        client_fd,
        response.data(),
        response.size()
    );
}

bool MockWebSocketServer::receive_client_frames(int client_fd)
{
    std::string fragmented_payload;
    bool fragmented_binary = false;
    bool fragmented_active = false;

    while (running_.load()) {
        std::array<unsigned char, 2> header{};

        if (!receive_exact(client_fd, header.data(), header.size())) {
            return false;
        }

        const bool final_frame = (header[0] & 0x80) != 0;
        const unsigned char opcode = header[0] & 0x0f;
        const bool masked = (header[1] & 0x80) != 0;

        std::uint64_t payload_length = header[1] & 0x7f;

        if (!masked || (header[0] & 0x70) != 0) {
            return false;
        }

        if (payload_length == 126) {
            std::array<unsigned char, 2> extended{};

            if (!receive_exact(
                client_fd,
                extended.data(),
                extended.size()
            )) {
                return false;
            }

            payload_length =
                (static_cast<std::uint64_t>(extended[0]) << 8) |
                extended[1];
        } else if (payload_length == 127) {
            std::array<unsigned char, 8> extended{};

            if (!receive_exact(
                client_fd,
                extended.data(),
                extended.size()
            )) {
                return false;
            }

            if ((extended[0] & 0x80) != 0) {
                return false;
            }

            payload_length = 0;

            for (unsigned char byte : extended) {
                payload_length =
                    (payload_length << 8) | byte;
            }
        }

        if (payload_length > MAX_FRAME_SIZE) {
            return false;
        }

        if (opcode >= 0x8 &&
            (opcode > 0xA || !final_frame || payload_length > 125)) {
            return false;
        }

        if (opcode == 0x3 || opcode == 0x4 ||
            (opcode >= 0xB && opcode <= 0xF)) {
            return false;
        }

        std::array<unsigned char, 4> mask{};

        if (!receive_exact(client_fd, mask.data(), mask.size())) {
            return false;
        }

        std::string payload(
            static_cast<std::size_t>(payload_length),
            '\0'
        );

        if (payload_length > 0 &&
            !receive_exact(
                client_fd,
                payload.data(),
                payload.size()
            )) {
            return false;
        }

        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(
                static_cast<unsigned char>(payload[i]) ^ mask[i % 4]
            );
        }

        if (opcode == 0x8) {
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                closes_.push_back(payload);
            }

            queue_condition_.notify_all();

            // Echo the exact CLOSE payload using opcode 0x8.
            (void)send_server_frame(
                client_fd,
                0x8,
                true,
                payload
            );

            return false;
        }

        if (opcode == 0x9) {
            // RFC 6455: respond to PING with PONG carrying the same data.
            if (!send_server_frame(
                client_fd,
                0xA,
                true,
                payload
            )) {
                return false;
            }

            continue;
        }

        if (opcode == 0xA) {
            const auto timestamp =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count();

            std::fprintf(
                stderr,
                "[mock] PONG received: timestamp=%lld payload_size=%zu payload='%.*s'\n",
                static_cast<long long>(timestamp),
                         payload.size(),
                         static_cast<int>(payload.size()),
                         payload.data()
            );

            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                pongs_.push_back(payload);
            }

            queue_condition_.notify_all();
            continue;
        }

        if (opcode == 0x1 || opcode == 0x2) {
            if (fragmented_active) {
                return false;
            }

            fragmented_binary = opcode == 0x2;
            fragmented_payload = std::move(payload);
            fragmented_active = !final_frame;
        } else if (opcode == 0x0) {
            if (!fragmented_active) {
                return false;
            }

            if (payload.size() >
                MAX_FRAME_SIZE - fragmented_payload.size()) {
                return false;
            }

            fragmented_payload += payload;

            if (final_frame) {
                fragmented_active = false;
            }
        } else {
            return false;
        }

        if ((opcode == 0x1 || opcode == 0x2) && final_frame) {
            ReceivedMessage message;
            message.payload = std::move(fragmented_payload);
            message.binary = fragmented_binary;

            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                incoming_.push_back(std::move(message));
            }

            queue_condition_.notify_one();
            fragmented_payload.clear();
            fragmented_active = false;
        } else if (opcode == 0x0 && final_frame) {
            ReceivedMessage message;
            message.payload = std::move(fragmented_payload);
            message.binary = fragmented_binary;

            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                incoming_.push_back(std::move(message));
            }

            queue_condition_.notify_one();
            fragmented_payload.clear();
            fragmented_active = false;
        }
    }

    return false;
}

void MockWebSocketServer::run()
{
    while (running_.load()) {
        pollfd descriptor{};
        descriptor.fd = listen_fd_;
        descriptor.events = POLLIN;

        const int result = poll(
            &descriptor,
            1,
            POLL_INTERVAL_MS
        );

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            break;
        }

        if (result == 0) {
            continue;
        }

        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }

        if (!(descriptor.revents & POLLIN)) {
            continue;
        }

        const int client = accept(listen_fd_, nullptr, nullptr);

        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (!running_.load()) {
                break;
            }

            continue;
        }

        client_fd_.store(client);

        if (perform_handshake(client)) {
            connected_.store(true);
            (void)receive_client_frames(client);
        }

        connected_.store(false);

        {
            std::lock_guard<std::mutex> lock(send_mutex_);

            if (client_fd_.load() == client) {
                client_fd_.store(-1);
                shutdown(client, SHUT_RDWR);
                close(client);
            }
        }

        queue_condition_.notify_all();
    }
}
