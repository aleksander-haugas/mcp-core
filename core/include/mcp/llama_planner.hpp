#pragma once

#include "mcp/llama_client.hpp"
#include "mcp/plan_validator.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace mcp {

struct LlamaPlannerConfig {
    std::size_t max_goal_bytes = 16U * 1024U;
    std::size_t max_completion_bytes = 256U * 1024U;
};

class LlamaPlanner {
public:
    using CompletionFunction = std::function<bool(
        const std::vector<LlamaMessage>&,
        LlamaCompletion&,
        std::string&)>;

    LlamaPlanner(
        CompletionFunction completion,
        const PlanValidator& validator,
        const LlamaPlannerConfig& config = LlamaPlannerConfig{});

    // The client must outlive this planner.
    LlamaPlanner(
        LlamaCppClient& client,
        const PlanValidator& validator,
        const LlamaPlannerConfig& config = LlamaPlannerConfig{});

    // The referenced validator must outlive this planner.
    // output remains unchanged on failure.
    bool create_plan(
        const std::string& goal,
        ValidatedPlan& output,
        std::string& error) const;

private:
    CompletionFunction completion_;
    const PlanValidator& validator_;
    LlamaPlannerConfig config_;
};

} // namespace mcp
