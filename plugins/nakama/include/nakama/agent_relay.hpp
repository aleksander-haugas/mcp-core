#ifndef NAKAMA_AGENT_RELAY_HPP
#define NAKAMA_AGENT_RELAY_HPP

#include "nakama/websocket_transport.hpp"

#include <json-c/json.h>
#include <json-c/json_tokener.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <climits>
#include <memory>
#include <string>
#include <utility>

namespace nakama {

constexpr std::size_t AGENT_RELAY_MAX_BYTES = 4u * 1024u * 1024u;
constexpr std::size_t AGENT_ENVELOPE_MAX_BYTES = 4u * 1024u * 1024u;
constexpr std::size_t AGENT_RELAY_MAX_IDENTIFIER_BYTES = 256u;
constexpr int AGENT_RELAY_STREAM_MODE = 123;
constexpr const char* AGENT_RELAY_STREAM_LABEL = "mcp_agents";

enum class AgentRelayStatus {
    IGNORED,
    ACCEPTED,
    INVALID
};

struct AgentRelayMessage {
    std::string authenticated_principal;
    std::string envelope;
};

struct JsonObjectDeleter {
    void operator()(json_object* value) const noexcept
    {
        if (value != nullptr) {
            json_object_put(value);
        }
    }
};

using JsonObjectPtr = std::unique_ptr<json_object, JsonObjectDeleter>;

inline JsonObjectPtr parse_strict_json(const std::string& input)
{
    if (input.empty() ||
        input.size() > AGENT_RELAY_MAX_BYTES ||
        input.size() > static_cast<std::size_t>(INT_MAX)) {
        return {};
    }

    json_tokener* tokener = json_tokener_new();
    if (tokener == nullptr) {
        return {};
    }

    json_tokener_set_flags(
        tokener,
        JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8
    );

    json_object* parsed = json_tokener_parse_ex(
        tokener,
        input.data(),
        static_cast<int>(input.size())
    );

    const bool parse_ok =
        json_tokener_get_error(tokener) == json_tokener_success &&
        parsed != nullptr;

    const std::size_t parse_end =
        json_tokener_get_parse_end(tokener);

    json_tokener_free(tokener);

    if (!parse_ok || parse_end > input.size()) {
        if (parsed != nullptr) {
            json_object_put(parsed);
        }
        return {};
    }

    for (std::size_t i = parse_end; i < input.size(); ++i) {
        if (std::isspace(static_cast<unsigned char>(input[i])) == 0) {
            json_object_put(parsed);
            return {};
        }
    }

    return JsonObjectPtr(parsed);
}

inline bool get_json_string(
    json_object* object,
    const char* key,
    std::string& output
)
{
    json_object* value = nullptr;

    if (object == nullptr ||
        !json_object_object_get_ex(object, key, &value) ||
        value == nullptr ||
        json_object_get_type(value) != json_type_string) {
        return false;
    }

    const char* data = json_object_get_string(value);
    const int length = json_object_get_string_len(value);

    if (data == nullptr || length < 0) {
        return false;
    }

    output.assign(data, static_cast<std::size_t>(length));
    return output.find('\0') == std::string::npos;
}

inline bool get_optional_empty_string(
    json_object* object,
    const char* key
)
{
    json_object* value = nullptr;

    if (!json_object_object_get_ex(object, key, &value)) {
        return true;
    }

    if (value == nullptr ||
        json_object_get_type(value) != json_type_string) {
        return false;
    }

    const char* data = json_object_get_string(value);
    const int length = json_object_get_string_len(value);

    return data != nullptr && length == 0;
}

inline AgentRelayStatus parse_agent_relay_event(
    const WebSocketMessage& message,
    AgentRelayMessage& output,
    std::string& error
)
{
    output = {};
    error.clear();

    if (message.type != WebSocketMessageType::TEXT) {
        return AgentRelayStatus::IGNORED;
    }

    if (message.payload.empty() ||
        message.payload.size() > AGENT_RELAY_MAX_BYTES) {
        error = "Invalid or oversized Nakama realtime message";
        return AgentRelayStatus::INVALID;
    }

    auto root = parse_strict_json(message.payload);

    if (!root) {
        error = "Malformed Nakama realtime JSON message";
        return AgentRelayStatus::INVALID;
    }

    // Other valid realtime event types are not AgentBus messages.
    if (json_object_get_type(root.get()) != json_type_object) {
        return AgentRelayStatus::IGNORED;
    }

    json_object* stream_data = nullptr;

    if (!json_object_object_get_ex(
            root.get(),
            "streamData",
            &stream_data)) {
        return AgentRelayStatus::IGNORED;
    }

    if (stream_data == nullptr ||
        json_object_get_type(stream_data) != json_type_object) {
        error = "Malformed Nakama streamData event";
        return AgentRelayStatus::INVALID;
    }

    json_object* stream = nullptr;

    // The WebSocket is shared by multiple agents and streams.
    // Ignore events that cannot be identified as our exact stream.
    if (!json_object_object_get_ex(stream_data, "stream", &stream) ||
        stream == nullptr ||
        json_object_get_type(stream) != json_type_object) {
        return AgentRelayStatus::IGNORED;
    }

    json_object* mode = nullptr;

    if (!json_object_object_get_ex(stream, "mode", &mode) ||
        mode == nullptr ||
        json_object_get_type(mode) != json_type_int ||
        json_object_get_int64(mode) != AGENT_RELAY_STREAM_MODE) {
        return AgentRelayStatus::IGNORED;
    }

    std::string label;

    if (!get_json_string(stream, "label", label) ||
        label != AGENT_RELAY_STREAM_LABEL) {
        return AgentRelayStatus::IGNORED;
    }

    // The event is now positively identified as an MCP agent stream.
    // Enforce the server-originated StreamSend contract strictly.
    json_object* sender = nullptr;
    if (json_object_object_get_ex(stream_data, "sender", &sender)) {
        error = "Nakama agent relay unexpectedly contains a sender";
        return AgentRelayStatus::INVALID;
    }

    if (!get_optional_empty_string(stream, "subject") ||
        !get_optional_empty_string(stream, "subcontext")) {
        error = "Unexpected Nakama agent relay stream identity";
        return AgentRelayStatus::INVALID;
    }

    std::string relay_payload;

    if (!get_json_string(stream_data, "data", relay_payload) ||
        relay_payload.empty() ||
        relay_payload.size() > AGENT_RELAY_MAX_BYTES) {
        error = "Invalid or oversized Nakama agent relay data";
        return AgentRelayStatus::INVALID;
    }

    auto relay = parse_strict_json(relay_payload);

    if (!relay || json_object_get_type(relay.get()) != json_type_object) {
        error = "Malformed Nakama agent relay JSON";
        return AgentRelayStatus::INVALID;
    }

    std::string protocol;
    std::string user_id;
    std::string session_id;

    json_object* version = nullptr;

    if (!get_json_string(relay.get(), "protocol", protocol) ||
        protocol != "mcp_agents_relay" ||
        !json_object_object_get_ex(relay.get(), "version", &version) ||
        version == nullptr ||
        json_object_get_type(version) != json_type_int ||
        json_object_get_int(version) != 1 ||
        !get_json_string(relay.get(), "user_id", user_id) ||
        !get_json_string(relay.get(), "session_id", session_id) ||
        user_id.empty() ||
        session_id.empty() ||
        user_id.size() > AGENT_RELAY_MAX_IDENTIFIER_BYTES ||
        session_id.size() > AGENT_RELAY_MAX_IDENTIFIER_BYTES) {
        error = "Invalid Nakama agent relay identity or protocol version";
        return AgentRelayStatus::INVALID;
    }

    json_object* envelope = nullptr;

    if (!json_object_object_get_ex(
            relay.get(),
            "envelope",
            &envelope) ||
        envelope == nullptr ||
        json_object_get_type(envelope) != json_type_object) {
        error = "Nakama agent relay has no valid Agent envelope";
        return AgentRelayStatus::INVALID;
    }

    const char* serialized = json_object_to_json_string_ext(
        envelope,
        JSON_C_TO_STRING_PLAIN
    );

    if (serialized == nullptr) {
        error = "Could not serialize Nakama Agent envelope";
        return AgentRelayStatus::INVALID;
    }

    const std::size_t envelope_size = std::char_traits<char>::length(
        serialized
    );

    if (envelope_size == 0 ||
        envelope_size > AGENT_ENVELOPE_MAX_BYTES) {
        error = "Nakama Agent envelope exceeds the 4 MiB limit";
        return AgentRelayStatus::INVALID;
    }

    output.authenticated_principal = std::move(user_id);
    output.envelope.assign(serialized, envelope_size);

    return AgentRelayStatus::ACCEPTED;
}

} // namespace nakama

#endif
