#include "mcp/plan_submitter.hpp"

#include <chrono>
#include <string>

namespace mcp {

bool PlanSubmitter::submit(
    const ValidatedPlan& plan,
    Orchestrator& orchestrator,
    PlanSubmissionResult& result,
    std::string& error) const
{
    result = PlanSubmissionResult{};
    error.clear();

    if (plan.tasks.empty()) {
        error = "Cannot submit an empty plan";
        result.failed_task_index = 0;
        return false;
    }

    for (std::size_t index = 0; index < plan.tasks.size(); ++index) {
        const PlannedTask& planned = plan.tasks[index];

        TaskRequest request;
        request.method = planned.method;
        request.payload_json = planned.payload_json;
        request.required_capabilities =
            planned.required_capabilities;
        request.timeout =
            std::chrono::milliseconds(planned.timeout_ms);

        std::string task_id;
        std::string submit_error;

        if (!orchestrator.submit(request, task_id, submit_error)) {
            result.failed_task_index = index;
            error = "Plan task " + std::to_string(index) +
                " submission failed";

            if (!submit_error.empty()) {
                error += ": " + submit_error;
            }

            return false;
        }

        result.task_ids.push_back(std::move(task_id));
    }

    error.clear();
    return true;
}

} // namespace mcp
