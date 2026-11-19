#ifndef MCP_AGENT_PROTOCOL_HPP
#define MCP_AGENT_PROTOCOL_HPP

#include <cstddef>
#include <string>

namespace mcp {

enum class AgentMessageType {
    REQUEST,
    RESPONSE,
    EVENT,
    ERROR
};

struct AgentIdentity {
    std::string agent_id;
    std::string instance_id;
};

struct AgentMessage {
    AgentMessageType type = AgentMessageType::REQUEST;

    std::string message_id;

    std::string correlation_id;
    bool has_correlation_id = false;

    AgentIdentity source;

    AgentIdentity target;
    bool has_target = false;

    std::string method;
    bool has_method = false;

    // JSON completo del payload. Por defecto, un objeto vacío.
    std::string payload_json = "{}";

    // Timestamp UTC en formato RFC 3339.
    std::string timestamp;
};

namespace agent_protocol {

constexpr int VERSION = 1;
constexpr std::size_t MAX_MESSAGE_SIZE = 1024 * 1024;
constexpr std::size_t MAX_IDENTIFIER_SIZE = 256;
constexpr std::size_t MAX_METHOD_SIZE = 256;

const char* message_type_name(AgentMessageType type);

bool validate_message(
    const AgentMessage& message,
    std::string& error
);

bool parse_message(
    const std::string& input,
    AgentMessage& message,
    std::string& error
);

bool serialize_message(
    const AgentMessage& message,
    std::string& output,
    std::string& error
);

} // namespace agent_protocol
} // namespace mcp

#endif
