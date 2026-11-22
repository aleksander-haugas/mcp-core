#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace mcp {

struct PlanMethodPolicy {
    std::string method;
    std::vector<std::string> required_capabilities;
};

struct PlanValidatorConfig {
    std::size_t max_plan_bytes = 256U * 1024U;
    std::size_t max_tasks = 16;
    std::size_t max_payload_bytes = 64U * 1024U;
    unsigned int max_timeout_ms = 300000;

    // Trusted local policy. Never populated from an LLM response.
    std::vector<PlanMethodPolicy> allowed_methods;
};

struct PlannedTask {
    std::string method;
    std::string payload_json;
    std::vector<std::string> required_capabilities;
    unsigned int timeout_ms = 0;
};

struct ValidatedPlan {
    unsigned int version = 1;
    std::vector<PlannedTask> tasks;
};

class PlanValidator {
public:
    explicit PlanValidator(
        const PlanValidatorConfig& config = PlanValidatorConfig{});

    // Read-only trusted method catalogue for planner prompt construction.
    const std::vector<PlanMethodPolicy>& allowed_methods() const noexcept;

    // Parses and validates an untrusted LLM proposal.
    // On failure, output is left unchanged.
    bool validate(
        const std::string& proposal_json,
        ValidatedPlan& output,
        std::string& error) const;

private:
    PlanValidatorConfig config_;
};

} // namespace mcp
