#include "mcp/agent_protocol.hpp"

#include <json-c/json.h>

#include <cctype>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

namespace mcp {
namespace agent_protocol {
namespace {

using JsonPtr = std::unique_ptr<json_object, decltype(&json_object_put)>;
using TokenerPtr = std::unique_ptr<json_tokener, decltype(&json_tokener_free)>;

JsonPtr make_json_ptr(json_object* value)
{
    return JsonPtr(value, json_object_put);
}

bool decode_json_key(const std::string& token, std::string& key)
{
    TokenerPtr tokener(json_tokener_new(), json_tokener_free);
    if (!tokener) {
        return false;
    }

    json_tokener_set_flags(tokener.get(), JSON_TOKENER_STRICT);

    std::string input = token;
    input.push_back(' ');

    json_object* parsed = json_tokener_parse_ex(
        tokener.get(), input.data(), static_cast<int>(input.size()));

    if (json_tokener_get_error(tokener.get()) != json_tokener_success ||
        parsed == nullptr ||
        json_object_get_type(parsed) != json_type_string) {
        if (parsed != nullptr) {
            json_object_put(parsed);
        }
        return false;
    }

    const char* value = json_object_get_string(parsed);
    const int length = json_object_get_string_len(parsed);

    if (value == nullptr || length < 0) {
        json_object_put(parsed);
        return false;
    }

    key.assign(value, static_cast<std::size_t>(length));
    json_object_put(parsed);
    return true;
}

void skip_json_whitespace(const std::string& input, std::size_t& pos)
{
    while (pos < input.size() &&
           std::isspace(static_cast<unsigned char>(input[pos]))) {
        ++pos;
    }
}

bool scan_json_string(const std::string& input, std::size_t& pos,
                      std::string& token)
{
    if (pos >= input.size() || input[pos] != '"') {
        return false;
    }

    const std::size_t start = pos++;

    while (pos < input.size()) {
        const char ch = input[pos++];

        if (ch == '"') {
            token.assign(input, start, pos - start);
            return true;
        }

        if (ch == '\\') {
            if (pos >= input.size()) {
                return false;
            }
            ++pos;
        }
    }

    return false;
}

bool scan_json_value(const std::string& input, std::size_t& pos,
                     unsigned int depth, std::string& error);

bool scan_json_object(const std::string& input, std::size_t& pos,
                      unsigned int depth, std::string& error)
{
    if (depth > 128) {
        error = "JSON nesting depth exceeds 128 levels";
        return false;
    }

    ++pos;
    skip_json_whitespace(input, pos);

    if (pos < input.size() && input[pos] == '}') {
        ++pos;
        return true;
    }

    std::unordered_set<std::string> keys;

    while (pos < input.size()) {
        std::string token;
        std::string key;

        if (!scan_json_string(input, pos, token) ||
            !decode_json_key(token, key)) {
            error = "Invalid JSON object key";
            return false;
        }

        if (!keys.insert(key).second) {
            error = "Duplicate JSON object key";
            return false;
        }

        skip_json_whitespace(input, pos);

        if (pos >= input.size() || input[pos] != ':') {
            error = "Invalid JSON object separator";
            return false;
        }

        ++pos;
        skip_json_whitespace(input, pos);

        if (!scan_json_value(input, pos, depth, error)) {
            return false;
        }

        skip_json_whitespace(input, pos);

        if (pos >= input.size()) {
            error = "Unterminated JSON object";
            return false;
        }

        if (input[pos] == '}') {
            ++pos;
            return true;
        }

        if (input[pos] != ',') {
            error = "Invalid JSON object delimiter";
            return false;
        }

        ++pos;
        skip_json_whitespace(input, pos);
    }

    error = "Unterminated JSON object";
    return false;
}

bool scan_json_array(const std::string& input, std::size_t& pos,
                     unsigned int depth, std::string& error)
{
    if (depth > 128) {
        error = "JSON nesting depth exceeds 128 levels";
        return false;
    }

    ++pos;
    skip_json_whitespace(input, pos);

    if (pos < input.size() && input[pos] == ']') {
        ++pos;
        return true;
    }

    while (pos < input.size()) {
        if (!scan_json_value(input, pos, depth, error)) {
            return false;
        }

        skip_json_whitespace(input, pos);

        if (pos >= input.size()) {
            error = "Unterminated JSON array";
            return false;
        }

        if (input[pos] == ']') {
            ++pos;
            return true;
        }

        if (input[pos] != ',') {
            error = "Invalid JSON array delimiter";
            return false;
        }

        ++pos;
        skip_json_whitespace(input, pos);
    }

    error = "Unterminated JSON array";
    return false;
}

bool scan_json_value(const std::string& input, std::size_t& pos,
                     unsigned int depth, std::string& error)
{
    skip_json_whitespace(input, pos);

    if (pos >= input.size()) {
        error = "Missing JSON value";
        return false;
    }

    if (input[pos] == '{') {
        return scan_json_object(input, pos, depth + 1, error);
    }

    if (input[pos] == '[') {
        return scan_json_array(input, pos, depth + 1, error);
    }

    if (input[pos] == '"') {
        std::string token;
        if (!scan_json_string(input, pos, token)) {
            error = "Invalid JSON string";
            return false;
        }
        return true;
    }

    const std::size_t start = pos;

    while (pos < input.size() &&
           input[pos] != ',' &&
           input[pos] != ']' &&
           input[pos] != '}' &&
           !std::isspace(static_cast<unsigned char>(input[pos]))) {
        ++pos;
    }

    if (pos == start) {
        error = "Invalid JSON value";
        return false;
    }

    return true;
}

bool reject_duplicate_json_keys(const std::string& input,
                                std::string& error)
{
    std::size_t pos = 0;

    if (!scan_json_value(input, pos, 0, error)) {
        return false;
    }

    skip_json_whitespace(input, pos);

    if (pos != input.size()) {
        error = "Unexpected trailing data after JSON document";
        return false;
    }

    return true;
}

bool parse_json_document(const std::string& input, JsonPtr& output,
                         std::string& error)
{
    if (input.empty() || input.size() > MAX_MESSAGE_SIZE) {
        error = "JSON document is empty or exceeds the size limit";
        return false;
    }

    if (!reject_duplicate_json_keys(input, error)) {
        return false;
    }

    TokenerPtr tokener(json_tokener_new(), json_tokener_free);
    if (!tokener) {
        error = "Unable to allocate JSON tokener";
        return false;
    }

    json_tokener_set_flags(tokener.get(), JSON_TOKENER_STRICT);

    // json-c needs a terminating character to recognize the end
    // of scalar JSON values such as null, true, false and numbers.
    std::string parse_input = input;
    parse_input.push_back(' ');

    json_object* parsed = json_tokener_parse_ex(
        tokener.get(), parse_input.data(),
        static_cast<int>(parse_input.size()));

    const auto status = json_tokener_get_error(tokener.get());
    const std::size_t parsed_length =
        json_tokener_get_parse_end(tokener.get());

    if (status != json_tokener_success) {
        if (parsed != nullptr) {
            json_object_put(parsed);
        }
        error = json_tokener_error_desc(status);
        return false;
    }

    for (std::size_t i = parsed_length; i < input.size(); ++i) {
        if (!std::isspace(static_cast<unsigned char>(input[i]))) {
            if (parsed != nullptr) {
                json_object_put(parsed);
            }
            error = "Unexpected trailing data after JSON document";
            return false;
        }
    }

    output.reset(parsed);
    return true;
}

bool get_string(json_object* object, const char* key,
                std::string& output, bool required,
                std::string& error)
{
    json_object* value = nullptr;

    if (!json_object_object_get_ex(object, key, &value)) {
        if (required) {
            error = std::string("Missing field: ") + key;
            return false;
        }
        output.clear();
        return true;
    }

    if (value == nullptr ||
        json_object_get_type(value) != json_type_string) {
        error = std::string("Field must be a string: ") + key;
        return false;
    }

    const char* raw = json_object_get_string(value);
    const int length = json_object_get_string_len(value);

    if (raw == nullptr || length < 0) {
        error = std::string("Invalid string field: ") + key;
        return false;
    }

    output.assign(raw, static_cast<std::size_t>(length));
    return true;
}

bool valid_identifier(const std::string& value)
{
    if (value.empty() || value.size() > MAX_IDENTIFIER_SIZE) {
        return false;
    }

    for (unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7f) {
            return false;
        }
    }

    return true;
}

bool valid_method(const std::string& value)
{
    if (value.empty() || value.size() > MAX_METHOD_SIZE) {
        return false;
    }

    bool segment_start = true;
    unsigned int segments = 0;

    for (unsigned char ch : value) {
        if (ch == '.') {
            if (segment_start) {
                return false;
            }

            segment_start = true;
            ++segments;
            continue;
        }

        if (segment_start) {
            if (!std::isalpha(ch) && ch != '_') {
                return false;
            }
            segment_start = false;
        } else if (!std::isalnum(ch) && ch != '_' && ch != '-') {
            return false;
        }
    }

    return !segment_start && segments >= 1;
}

bool validate_error_payload(json_object* payload, std::string& error)
{
    if (payload == nullptr ||
        json_object_get_type(payload) != json_type_object) {
        error = "Error payload must be a JSON object";
        return false;
    }

    std::string code;
    std::string message;

    if (!get_string(payload, "code", code, true, error) ||
        !get_string(payload, "message", message, true, error)) {
        return false;
    }

    if (code.empty() || code.size() > 64 ||
        code[0] < 'A' || code[0] > 'Z') {
        error = "Invalid error code";
        return false;
    }

    for (unsigned char ch : code) {
        if (!((ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_')) {
            error = "Invalid error code";
            return false;
        }
    }

    if (message.empty() || message.size() > 4096) {
        error = "Invalid error message";
        return false;
    }

    json_object* details = nullptr;
    if (json_object_object_get_ex(payload, "details", &details) &&
        (details == nullptr ||
         json_object_get_type(details) != json_type_object)) {
        error = "Error details must be a JSON object";
        return false;
    }

    return true;
}

bool is_digits(const std::string& value, std::size_t offset,
               std::size_t count)
{
    if (offset + count > value.size()) {
        return false;
    }

    for (std::size_t i = offset; i < offset + count; ++i) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
    }

    return true;
}

int number_at(const std::string& value, std::size_t offset,
              std::size_t count)
{
    int result = 0;

    for (std::size_t i = offset; i < offset + count; ++i) {
        result = result * 10 + (value[i] - '0');
    }

    return result;
}

bool valid_timestamp(const std::string& value)
{
    // UTC RFC 3339: YYYY-MM-DDTHH:MM:SS[.fraction]Z
    if (value.size() < 20 ||
        !is_digits(value, 0, 4) ||
        value[4] != '-' ||
        !is_digits(value, 5, 2) ||
        value[7] != '-' ||
        !is_digits(value, 8, 2) ||
        value[10] != 'T' ||
        !is_digits(value, 11, 2) ||
        value[13] != ':' ||
        !is_digits(value, 14, 2) ||
        value[16] != ':' ||
        !is_digits(value, 17, 2)) {
        return false;
    }

    const int year = number_at(value, 0, 4);
    const int month = number_at(value, 5, 2);
    const int day = number_at(value, 8, 2);
    const int hour = number_at(value, 11, 2);
    const int minute = number_at(value, 14, 2);
    const int second = number_at(value, 17, 2);

    if (year == 0 || month < 1 || month > 12 ||
        hour > 23 || minute > 59 || second > 59) {
        return false;
    }

    static constexpr int days_per_month[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    int max_day = days_per_month[month - 1];

    const bool leap_year =
        (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;

    if (month == 2 && leap_year) {
        max_day = 29;
    }

    if (day < 1 || day > max_day) {
        return false;
    }

    std::size_t pos = 19;

    if (pos < value.size() && value[pos] == '.') {
        ++pos;
        const std::size_t fraction_start = pos;

        while (pos < value.size() &&
               value[pos] >= '0' && value[pos] <= '9') {
            ++pos;
        }

        if (pos == fraction_start) {
            return false;
        }
    }

    return pos + 1 == value.size() && value[pos] == 'Z';
}

bool parse_identity(json_object* parent, const char* key,
                    AgentIdentity& identity, std::string& error)
{
    json_object* object = nullptr;

    if (!json_object_object_get_ex(parent, key, &object) ||
        object == nullptr ||
        json_object_get_type(object) != json_type_object) {
        error = std::string("Missing or invalid identity: ") + key;
        return false;
    }

    if (!get_string(object, "agent_id", identity.agent_id,
                    true, error) ||
        !get_string(object, "instance_id", identity.instance_id,
                    true, error)) {
        return false;
    }

    if (!valid_identifier(identity.agent_id) ||
        !valid_identifier(identity.instance_id)) {
        error = std::string("Invalid identity values: ") + key;
        return false;
    }

    return true;
}

bool add_string(json_object* object, const char* key,
                const std::string& value, std::string& error)
{
    json_object* item = json_object_new_string_len(
        value.data(), static_cast<int>(value.size()));

    if (item == nullptr) {
        error = std::string("Unable to allocate field: ") + key;
        return false;
    }

    if (json_object_object_add(object, key, item) != 0) {
        json_object_put(item);
        error = std::string("Unable to add field: ") + key;
        return false;
    }

    return true;
}

bool add_identity(json_object* parent, const char* key,
                  const AgentIdentity& identity, std::string& error)
{
    JsonPtr object = make_json_ptr(json_object_new_object());

    if (!object) {
        error = "Unable to allocate identity object";
        return false;
    }

    if (!add_string(object.get(), "agent_id",
                    identity.agent_id, error) ||
        !add_string(object.get(), "instance_id",
                    identity.instance_id, error)) {
        return false;
    }

    json_object* raw = object.release();

    if (json_object_object_add(parent, key, raw) != 0) {
        json_object_put(raw);
        error = std::string("Unable to add identity: ") + key;
        return false;
    }

    return true;
}

} // namespace

const char* message_type_name(AgentMessageType type)
{
    switch (type) {
        case AgentMessageType::REQUEST:
            return "request";
        case AgentMessageType::RESPONSE:
            return "response";
        case AgentMessageType::EVENT:
            return "event";
        case AgentMessageType::ERROR:
            return "error";
    }

    return nullptr;
}

bool validate_message(const AgentMessage& message, std::string& error)
{
    error.clear();

    if (message_type_name(message.type) == nullptr) {
        error = "Unsupported message type";
        return false;
    }

    if (!valid_identifier(message.message_id)) {
        error = "Invalid message_id";
        return false;
    }

    if (!valid_identifier(message.source.agent_id) ||
        !valid_identifier(message.source.instance_id)) {
        error = "Invalid source identity";
        return false;
    }

    if (message.has_target &&
        (!valid_identifier(message.target.agent_id) ||
         !valid_identifier(message.target.instance_id))) {
        error = "Invalid target identity";
        return false;
    }

    if (message.has_correlation_id &&
        !valid_identifier(message.correlation_id)) {
        error = "Invalid correlation_id";
        return false;
    }

    if (message.has_method && !valid_method(message.method)) {
        error = "Invalid method";
        return false;
    }

    if (!valid_timestamp(message.timestamp)) {
        error = "Invalid UTC RFC 3339 timestamp";
        return false;
    }

    switch (message.type) {
        case AgentMessageType::REQUEST:
            if (!message.has_target || !message.has_method ||
                message.has_correlation_id) {
                error = "Request requires target and method; "
                        "correlation_id is not permitted";
                return false;
            }
            break;

        case AgentMessageType::RESPONSE:
            if (!message.has_target || !message.has_correlation_id ||
                message.has_method) {
                error = "Response requires target and correlation_id; "
                        "method is not permitted";
                return false;
            }
            break;

        case AgentMessageType::EVENT:
            if (!message.has_method || message.has_correlation_id) {
                error = "Event requires method; "
                        "correlation_id is not permitted";
                return false;
            }
            break;

        case AgentMessageType::ERROR:
            if (!message.has_target || !message.has_correlation_id ||
                message.has_method) {
                error = "Error requires target and correlation_id; "
                        "method is not permitted";
                return false;
            }
            break;
    }

    JsonPtr payload(nullptr, json_object_put);

    if (!parse_json_document(message.payload_json, payload, error)) {
        error = "Invalid payload: " + error;
        return false;
    }

    if (message.type == AgentMessageType::ERROR &&
        !validate_error_payload(payload.get(), error)) {
        return false;
    }

    return true;
}

bool parse_message(const std::string& input, AgentMessage& message,
                  std::string& error)
{
    error.clear();

    if (input.size() > MAX_MESSAGE_SIZE) {
        error = "Message exceeds maximum size";
        return false;
    }

    JsonPtr document(nullptr, json_object_put);

    if (!parse_json_document(input, document, error)) {
        return false;
    }

    if (!document ||
        json_object_get_type(document.get()) != json_type_object) {
        error = "Agent message must be a JSON object";
        return false;
    }

    AgentMessage parsed;

    std::string protocol;
    if (!get_string(document.get(), "protocol", protocol, true, error) ||
        protocol != "agent") {
        error = "Invalid or missing protocol identifier";
        return false;
    }

    json_object* version = nullptr;

    if (!json_object_object_get_ex(document.get(), "version", &version) ||
        version == nullptr ||
        json_object_get_type(version) != json_type_int ||
        json_object_get_int(version) != VERSION) {
        error = "Unsupported or missing Agent Protocol version";
        return false;
    }

    std::string type;
    if (!get_string(document.get(), "type", type, true, error)) {
        return false;
    }

    if (type == "request") {
        parsed.type = AgentMessageType::REQUEST;
    } else if (type == "response") {
        parsed.type = AgentMessageType::RESPONSE;
    } else if (type == "event") {
        parsed.type = AgentMessageType::EVENT;
    } else if (type == "error") {
        parsed.type = AgentMessageType::ERROR;
    } else {
        error = "Unknown Agent Protocol message type";
        return false;
    }

    if (!get_string(document.get(), "message_id",
                    parsed.message_id, true, error) ||
        !parse_identity(document.get(), "source", parsed.source, error)) {
        return false;
    }

    json_object* target = nullptr;
    if (json_object_object_get_ex(document.get(), "target", &target)) {
        if (!parse_identity(document.get(), "target",
                            parsed.target, error)) {
            return false;
        }
        parsed.has_target = true;
    }

    json_object* correlation = nullptr;
    if (json_object_object_get_ex(
            document.get(), "correlation_id", &correlation)) {
        if (!get_string(document.get(), "correlation_id",
                        parsed.correlation_id, true, error)) {
            return false;
        }
        parsed.has_correlation_id = true;
    }

    json_object* method = nullptr;
    if (json_object_object_get_ex(document.get(), "method", &method)) {
        if (!get_string(document.get(), "method",
                        parsed.method, true, error)) {
            return false;
        }
        parsed.has_method = true;
    }

    json_object* payload = nullptr;
    if (!json_object_object_get_ex(document.get(), "payload", &payload)) {
        error = "Missing payload";
        return false;
    }

    const char* payload_text = payload == nullptr
        ? "null"
        : json_object_to_json_string_ext(
              payload, JSON_C_TO_STRING_PLAIN);

    if (payload_text == nullptr) {
        error = "Unable to serialize payload";
        return false;
    }

    parsed.payload_json = payload_text;

    if (!get_string(document.get(), "timestamp",
                    parsed.timestamp, true, error)) {
        return false;
    }

    if (!validate_message(parsed, error)) {
        return false;
    }

    message = std::move(parsed);
    return true;
}

bool serialize_message(const AgentMessage& message, std::string& output,
                       std::string& error)
{
    error.clear();

    if (!validate_message(message, error)) {
        return false;
    }

    JsonPtr root = make_json_ptr(json_object_new_object());

    if (!root) {
        error = "Unable to allocate Agent Protocol envelope";
        return false;
    }

    if (!add_string(root.get(), "protocol", "agent", error)) {
        return false;
    }

    json_object* version = json_object_new_int(VERSION);
    if (version == nullptr ||
        json_object_object_add(root.get(), "version", version) != 0) {
        if (version != nullptr) {
            json_object_put(version);
        }
        error = "Unable to add protocol version";
        return false;
    }

    if (!add_string(root.get(), "type",
                    message_type_name(message.type), error) ||
        !add_string(root.get(), "message_id",
                    message.message_id, error) ||
        !add_identity(root.get(), "source", message.source, error)) {
        return false;
    }

    if (message.has_target &&
        !add_identity(root.get(), "target", message.target, error)) {
        return false;
    }

    if (message.has_correlation_id &&
        !add_string(root.get(), "correlation_id",
                    message.correlation_id, error)) {
        return false;
    }

    if (message.has_method &&
        !add_string(root.get(), "method", message.method, error)) {
        return false;
    }

    JsonPtr payload(nullptr, json_object_put);
    if (!parse_json_document(message.payload_json, payload, error)) {
        error = "Invalid payload: " + error;
        return false;
    }

    json_object* raw_payload = payload.release();

    if (json_object_object_add(root.get(), "payload", raw_payload) != 0) {
        if (raw_payload != nullptr) {
            json_object_put(raw_payload);
        }
        error = "Unable to add payload";
        return false;
    }

    if (!add_string(root.get(), "timestamp",
                    message.timestamp, error)) {
        return false;
    }

    const char* serialized = json_object_to_json_string_ext(
        root.get(), JSON_C_TO_STRING_PLAIN);

    if (serialized == nullptr) {
        error = "Unable to serialize Agent Protocol envelope";
        return false;
    }

    std::string result(serialized);

    if (result.size() > MAX_MESSAGE_SIZE) {
        error = "Serialized message exceeds maximum size";
        return false;
    }

    output = std::move(result);
    return true;
}

} // namespace agent_protocol
} // namespace mcp
