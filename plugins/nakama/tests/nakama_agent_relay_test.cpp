#include "nakama/agent_relay.hpp"

#include <json-c/json.h>

#include <cstdio>
#include <string>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* description)
{
    ++checks;
    std::printf(
        "[%s] %s\n",
        condition ? "PASS" : "FAIL",
        description
    );

    if (!condition) {
        ++failures;
    }
}

std::string make_event(
    const std::string& relay,
    const char* extra_stream_fields = "",
    int stream_mode = nakama::AGENT_RELAY_STREAM_MODE,
    const char* stream_label = nakama::AGENT_RELAY_STREAM_LABEL
)
{
    json_object* root = json_object_new_object();
    json_object* stream_data = json_object_new_object();
    json_object* stream = json_object_new_object();

    json_object_object_add(
        stream,
        "mode",
        json_object_new_int(stream_mode)
    );
    json_object_object_add(
        stream,
        "label",
        json_object_new_string(stream_label)
    );

    json_object_object_add(stream_data, "stream", stream);
    json_object_object_add(
        stream_data,
        "data",
        json_object_new_string_len(
            relay.data(),
            static_cast<int>(relay.size())
        )
    );
    json_object_object_add(root, "streamData", stream_data);

    if (extra_stream_fields != nullptr &&
        *extra_stream_fields != '\0') {
        json_object_object_add(
            stream_data,
            "sender",
            json_object_new_string(extra_stream_fields)
        );
    }

    const char* encoded = json_object_to_json_string_ext(
        root,
        JSON_C_TO_STRING_PLAIN
    );

    std::string result = encoded != nullptr ? encoded : "";
    json_object_put(root);
    return result;
}

std::string make_relay(
    const std::string& envelope,
    const char* user_id = "authenticated-user",
    const char* session_id = "authenticated-session"
)
{
    json_object* relay = json_object_new_object();
    json_object_object_add(
        relay,
        "protocol",
        json_object_new_string("mcp_agents_relay")
    );
    json_object_object_add(
        relay,
        "version",
        json_object_new_int(1)
    );
    json_object_object_add(
        relay,
        "user_id",
        json_object_new_string(user_id)
    );
    json_object_object_add(
        relay,
        "session_id",
        json_object_new_string(session_id)
    );

    json_tokener* tokener = json_tokener_new();
    json_object* parsed = json_tokener_parse_ex(
        tokener,
        envelope.data(),
        static_cast<int>(envelope.size())
    );

    const bool valid =
        json_tokener_get_error(tokener) == json_tokener_success &&
        parsed != nullptr;

    json_tokener_free(tokener);

    if (!valid) {
        if (parsed != nullptr) {
            json_object_put(parsed);
        }
        json_object_put(relay);
        return {};
    }

    json_object_object_add(relay, "envelope", parsed);

    const char* encoded = json_object_to_json_string_ext(
        relay,
        JSON_C_TO_STRING_PLAIN
    );

    std::string result = encoded != nullptr ? encoded : "";
    json_object_put(relay);
    return result;
}

nakama::AgentRelayStatus parse(
    const std::string& payload,
    nakama::AgentRelayMessage& output,
    std::string& error
)
{
    nakama::WebSocketMessage message;
    message.type = nakama::WebSocketMessageType::TEXT;
    message.payload = payload;

    return nakama::parse_agent_relay_event(message, output, error);
}

} // namespace

int main()
{
    nakama::AgentRelayMessage output;
    std::string error;

    const std::string valid_relay = make_relay(
        R"({"protocol":"agent","version":1,"type":"event","message_id":"m1","source":{"agent_id":"monitor","instance_id":"monitor-1"}})"
    );

    check(
        parse(
            make_event(valid_relay),
            output,
            error
        ) == nakama::AgentRelayStatus::ACCEPTED,
        "accepts valid authenticated streamData relay"
    );

    check(
        output.authenticated_principal == "authenticated-user",
        "uses authenticated relay user_id as principal"
    );

    check(
        !output.envelope.empty(),
        "extracts the inner Agent envelope"
    );

    check(
        parse(
            R"({"presence":{"joins":[]}})",
            output,
            error
        ) == nakama::AgentRelayStatus::IGNORED,
        "ignores unrelated realtime events"
    );

    nakama::WebSocketMessage binary;
    binary.type = nakama::WebSocketMessageType::BINARY;
    binary.payload = "{}";

    check(
        nakama::parse_agent_relay_event(binary, output, error) ==
            nakama::AgentRelayStatus::IGNORED,
        "ignores binary WebSocket messages"
    );

    check(
        parse(
            make_event(valid_relay, "unexpected-sender"),
            output,
            error
        ) == nakama::AgentRelayStatus::INVALID,
        "rejects a relay containing sender"
    );

    check(
        parse(
            make_event(valid_relay, "foreign-sender", 124, "other_stream"),
            output,
            error
        ) == nakama::AgentRelayStatus::IGNORED,
        "ignores another stream even when it contains sender"
    );

    const std::string agent_a_envelope =
        R"({"protocol":"agent","version":1,"type":"message","message_id":"a-to-b","source":{"agent_id":"agent-a","instance_id":"a-1"},"target":{"agent_id":"agent-b","instance_id":"b-1"}})";
    const std::string agent_b_envelope =
        R"({"protocol":"agent","version":1,"type":"message","message_id":"b-to-c","source":{"agent_id":"agent-b","instance_id":"b-1"},"target":{"agent_id":"agent-c","instance_id":"c-1"}})";

    check(
        parse(
            make_event(make_relay(
                agent_a_envelope, "nakama-user-a", "nakama-session-a")),
            output,
            error
        ) == nakama::AgentRelayStatus::ACCEPTED &&
        output.authenticated_principal == "nakama-user-a" &&
        output.envelope.find("\\"agent-b\\"") != std::string::npos,
        "preserves agent A identity and its distinct agent B destination"
    );

    check(
        parse(
            make_event(make_relay(
                agent_b_envelope, "nakama-user-b", "nakama-session-b")),
            output,
            error
        ) == nakama::AgentRelayStatus::ACCEPTED &&
        output.authenticated_principal == "nakama-user-b" &&
        output.envelope.find("\\"agent-c\\"") != std::string::npos,
        "preserves agent B identity and its distinct agent C destination"
    );

    std::string wrong_mode = make_event(valid_relay);
    const std::string mode_text = "\"mode\":123";
    const std::size_t mode_pos = wrong_mode.find(mode_text);
    if (mode_pos != std::string::npos) {
        wrong_mode.replace(mode_pos, mode_text.size(), "\"mode\":124");
    }

    check(
        parse(wrong_mode, output, error) ==
            nakama::AgentRelayStatus::IGNORED,
        "ignores streamData belonging to another stream mode"
    );

    std::string wrong_label = make_event(valid_relay);
    const std::string label_text = "mcp_agents";
    const std::size_t label_pos = wrong_label.find(label_text);
    if (label_pos != std::string::npos) {
        wrong_label.replace(label_pos, label_text.size(), "other_agent");
    }

    check(
        parse(wrong_label, output, error) ==
            nakama::AgentRelayStatus::IGNORED,
        "ignores streamData belonging to another stream label"
    );

    check(
        parse(
            make_event("{invalid"),
            output,
            error
        ) == nakama::AgentRelayStatus::INVALID,
        "rejects malformed relay JSON"
    );

    const std::string large_envelope =
        "{\"payload\":\"" +
        std::string(3u * 1024u * 1024u, 'x') +
        "\"}";

    const std::string large_relay = make_relay(large_envelope);

    check(
        !large_relay.empty() &&
        large_relay.size() < nakama::AGENT_RELAY_MAX_BYTES &&
        parse(
            make_event(large_relay),
            output,
            error
        ) == nakama::AgentRelayStatus::ACCEPTED,
        "accepts a multi-megabyte envelope below the 4 MiB relay limit"
    );

    const std::string oversized_envelope =
        "{\"payload\":\"" +
        std::string(4u * 1024u * 1024u, 'x') +
        "\"}";

    check(
        parse(
            make_event(make_relay(oversized_envelope)),
            output,
            error
        ) == nakama::AgentRelayStatus::INVALID,
        "rejects a relay exceeding the 4 MiB WebSocket limit"
    );

    std::printf(
        "\nChecks: %d, failures: %d\n",
        checks,
        failures
    );

    return failures == 0 ? 0 : 1;
}
