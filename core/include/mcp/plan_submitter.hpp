#pragma once

#include "mcp/orchestrator.hpp"
#include "mcp/plan_validator.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace mcp {

struct PlanSubmissionResult {
    std::vector<std::string> task_ids;

    // Index of the task whose submission failed.
    // SIZE_MAX means that no task failed.
    std::size_t failed_task_index = static_cast<std::size_t>(-1);
};

class PlanSubmitter {
public:
    // Submits tasks sequentially.
    // On partial failure, result retains all accepted task IDs.
    bool submit(
        const ValidatedPlan& plan,
        Orchestrator& orchestrator,
        PlanSubmissionResult& result,
        std::string& error
    ) const;
};

} // namespace mcp
