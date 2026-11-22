#include "mcp/plan_validator.hpp"

#include "mcp/agent_protocol.hpp"

#include <json-c/json.h>

#include <cctype>
#include <climits>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace mcp {
namespace {

using JsonPtr = std::unique_ptr<json_object, decltype(&json_object_put)>;

bool valid_name(const std::string& value)
{
    if (value.empty() || value.size() > agent_protocol::MAX_METHOD_SIZE) {
        return false;
    }

    for (unsigned char ch : value) {
        if (!(std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

bool valid_config(
    const PlanValidatorConfig& config,
    std::map<std::string, std::vector<std::string>>& policies,
    std::string& error)
{
    constexpr std::size_t max_config_bytes = 64U * 1024U * 1024U;

    if (config.max_plan_bytes == 0 ||
        config.max_plan_bytes > max_config_bytes ||
        config.max_plan_bytes > static_cast<std::size_t>(INT_MAX)) {
        error = "max_plan_bytes is outside the permitted range";
        return false;
    }

    if (config.max_tasks == 0 || config.max_tasks > 256) {
        error = "max_tasks must be between 1 and 256";
        return false;
    }

    if (config.max_payload_bytes == 0 ||
        config.max_payload_bytes > max_config_bytes) {
        error = "max_payload_bytes is outside the permitted range";
        return false;
    }

    if (config.max_timeout_ms == 0 ||
        config.max_timeout_ms > 300000 ||
        config.max_timeout_ms > UINT_MAX) {
        error = "max_timeout_ms must be between 1 and 300000";
        return false;
    }

    if (config.allowed_methods.empty()) {
        error = "trusted method policy is empty";
        return false;
    }

    for (const auto& policy : config.allowed_methods) {
        if (!valid_name(policy.method)) {
            error = "trusted policy contains an invalid method name";
            return false;
        }

        std::set<std::string> unique_capabilities;
        for (const auto& capability : policy.required_capabilities) {
            if (!valid_name(capability) ||
                !unique_capabilities.insert(capability).second) {
                error = "trusted policy contains an invalid or duplicate capability";
                return false;
            }
        }

        if (!policies.emplace(
                policy.method, policy.required_capabilities).second) {
            error = "trusted policy contains a duplicate method";
            return false;
        }
    }

    return true;
}

bool has_only_keys(
    json_object* object,
    const std::set<std::string>& allowed)
{
    if (object == nullptr ||
        json_object_get_type(object) != json_type_object) {
        return false;
    }

    json_object_object_foreach(object, key, value) {
        (void)value;
        if (allowed.find(key) == allowed.end()) {
            return false;
        }
    }
    return true;
}

bool get_string(
    json_object* object,
    const char* key,
    std::string& output)
{
    json_object* value = nullptr;
    if (object == nullptr ||
        !json_object_object_get_ex(object, key, &value) ||
        value == nullptr ||
        json_object_get_type(value) != json_type_string) {
        return false;
    }

    const char* text = json_object_get_string(value);
    const int length = json_object_get_string_len(value);
    if (text == nullptr || length <= 0) {
        return false;
    }

    output.assign(text, static_cast<std::size_t>(length));
    return output.find('\0') == std::string::npos;
}

bool get_positive_integer(
    json_object* object,
    const char* key,
    unsigned int maximum,
    unsigned int& output)
{
    json_object* value = nullptr;
    if (object == nullptr ||
        !json_object_object_get_ex(object, key, &value) ||
        value == nullptr ||
        json_object_get_type(value) != json_type_int) {
        return false;
    }

    const std::int64_t number = json_object_get_int64(value);
    if (number <= 0 ||
        static_cast<std::uint64_t>(number) > maximum) {
        return false;
    }

    output = static_cast<unsigned int>(number);
    return true;
}

bool parse_document(
    const std::string& input,
    JsonPtr& document,
    std::string& error)
{
    json_tokener* raw_tokener = json_tokener_new();
    if (raw_tokener == nullptr) {
        error = "could not allocate plan JSON parser";
        return false;
    }

    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> tokener(
        raw_tokener, &json_tokener_free);

    json_tokener_set_flags(
        tokener.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);

    json_object* parsed = json_tokener_parse_ex(
        tokener.get(), input.data(), static_cast<int>(input.size()));

    const auto parse_error = json_tokener_get_error(tokener.get());
    const std::size_t parse_end =
        json_tokener_get_parse_end(tokener.get());

    if (parse_error != json_tokener_success || parsed == nullptr) {
        if (parsed != nullptr) {
            json_object_put(parsed);
        }
        error = "plan proposal contains invalid JSON";
        return false;
    }

    for (std::size_t i = parse_end; i < input.size(); ++i) {
        if (!std::isspace(static_cast<unsigned char>(input[i]))) {
            json_object_put(parsed);
            error = "plan proposal contains trailing non-whitespace data";
            return false;
        }
    }

    document.reset(parsed);
    return true;
}

} // namespace

PlanValidator::PlanValidator(const PlanValidatorConfig& config)
    : config_(config)
{
}

const std::vector<PlanMethodPolicy>&
PlanValidator::allowed_methods() const noexcept
{
    return config_.allowed_methods;
}

bool PlanValidator::validate(
    const std::string& proposal_json,
    ValidatedPlan& output,
    std::string& error) const
{
    error.clear();

    if (proposal_json.empty() ||
        proposal_json.size() > config_.max_plan_bytes ||
        proposal_json.size() > static_cast<std::size_t>(INT_MAX)) {
        error = "plan proposal is empty or exceeds max_plan_bytes";
        return false;
    }

    std::map<std::string, std::vector<std::string>> policies;
    if (!valid_config(config_, policies, error)) {
        return false;
    }

    JsonPtr document(nullptr, &json_object_put);
    if (!parse_document(proposal_json, document, error)) {
        return false;
    }

    if (!has_only_keys(document.get(), {"version", "tasks"})) {
        error = "plan root must contain only version and tasks";
        return false;
    }

    json_object* version = nullptr;
    if (!json_object_object_get_ex(document.get(), "version", &version) ||
        version == nullptr ||
        json_object_get_type(version) != json_type_int ||
        json_object_get_int64(version) != 1) {
        error = "unsupported or missing plan version";
        return false;
    }

    json_object* tasks = nullptr;
    if (!json_object_object_get_ex(document.get(), "tasks", &tasks) ||
        tasks == nullptr ||
        json_object_get_type(tasks) != json_type_array) {
        error = "plan tasks must be an array";
        return false;
    }

    const std::size_t task_count =
        json_object_array_length(tasks);
    if (task_count == 0 || task_count > config_.max_tasks) {
        error = "plan task count is outside the permitted range";
        return false;
    }

    ValidatedPlan candidate;
    candidate.version = 1;
    candidate.tasks.reserve(task_count);

    for (std::size_t i = 0; i < task_count; ++i) {
        json_object* item = json_object_array_get_idx(tasks, i);
        if (!has_only_keys(
                item, {"method", "payload", "timeout_ms"})) {
            error = "task contains unknown fields or is not an object";
            return false;
        }

        PlannedTask task;
        if (!get_string(item, "method", task.method) ||
            !valid_name(task.method)) {
            error = "task contains an invalid or missing method";
            return false;
        }

        const auto policy = policies.find(task.method);
        if (policy == policies.end()) {
            error = "plan requests a method not present in trusted policy";
            return false;
        }

        json_object* payload = nullptr;
        if (!json_object_object_get_ex(item, "payload", &payload) ||
            payload == nullptr ||
            json_object_get_type(payload) != json_type_object) {
            error = "task payload must be a JSON object";
            return false;
        }

        const char* payload_text = json_object_to_json_string_ext(
            payload, JSON_C_TO_STRING_PLAIN);
        if (payload_text == nullptr) {
            error = "could not serialize task payload";
            return false;
        }

        task.payload_json.assign(payload_text);
        if (task.payload_json.size() > config_.max_payload_bytes) {
            error = "task payload exceeds max_payload_bytes";
            return false;
        }

        if (!get_positive_integer(
                item, "timeout_ms", config_.max_timeout_ms,
                task.timeout_ms)) {
            error = "task timeout_ms is missing or outside the permitted range";
            return false;
        }

        // Capabilities are derived only from trusted local policy.
        task.required_capabilities = policy->second;
        candidate.tasks.push_back(std::move(task));
    }

    output = std::move(candidate);
    return true;
}

} // namespace mcp
