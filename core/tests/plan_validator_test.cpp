#include "mcp/plan_validator.hpp"

#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using Test = std::pair<std::string, std::function<bool()>>;

mcp::PlanValidator make_validator()
{
    mcp::PlanValidatorConfig config;
    config.allowed_methods = {
        {"system.inspect", {"system.read"}},
        {"system.configure", {"system.write"}}
    };
    return mcp::PlanValidator(config);
}

bool rejected(const std::string& json)
{
    auto validator = make_validator();
    mcp::ValidatedPlan output;
    std::string error;
    return !validator.validate(json, output, error) && !error.empty();
}

} // namespace

int main()
{
    const std::string valid =
        R"({"version":1,"tasks":[{"method":"system.inspect","payload":{"target":"memory"},"timeout_ms":10000}]})";

    std::vector<Test> tests;

    tests.emplace_back("valid plan", [=] {
        auto validator = make_validator();
        mcp::ValidatedPlan output;
        std::string error;
        if (!validator.validate(valid, output, error)) return false;
        return output.version == 1 &&
               output.tasks.size() == 1 &&
               output.tasks[0].method == "system.inspect" &&
               output.tasks[0].payload_json == R"({"target":"memory"})" &&
               output.tasks[0].timeout_ms == 10000 &&
               output.tasks[0].required_capabilities ==
                   std::vector<std::string>{"system.read"};
    });

    tests.emplace_back("malformed JSON", [] {
        return rejected(R"({"version":1,"tasks":[)");
    });

    tests.emplace_back("trailing non-whitespace data", [] {
        return rejected(
            R"({"version":1,"tasks":[{"method":"system.inspect","payload":{},"timeout_ms":1000}]} garbage)");
    });

    tests.emplace_back("unsupported plan version", [] {
        return rejected(
            R"({"version":2,"tasks":[{"method":"system.inspect","payload":{},"timeout_ms":1000}]})");
    });

    tests.emplace_back("untrusted method rejected", [] {
        return rejected(
            R"({"version":1,"tasks":[{"method":"system.delete_all","payload":{},"timeout_ms":1000}]})");
    });

    tests.emplace_back("model cannot inject capabilities", [] {
        return rejected(
            R"({"version":1,"tasks":[{"method":"system.inspect","required_capabilities":["system.admin"],"payload":{},"timeout_ms":1000}]})");
    });

    tests.emplace_back("unknown root field rejected", [] {
        return rejected(
            R"({"version":1,"agent":"attacker","tasks":[{"method":"system.inspect","payload":{},"timeout_ms":1000}]})");
    });

    tests.emplace_back("unknown task field rejected", [] {
        return rejected(
            R"({"version":1,"tasks":[{"method":"system.inspect","payload":{},"timeout_ms":1000,"agent_id":"attacker"}]})");
    });

    tests.emplace_back("timeout zero rejected", [] {
        return rejected(
            R"({"version":1,"tasks":[{"method":"system.inspect","payload":{},"timeout_ms":0}]})");
    });

    tests.emplace_back("timeout above policy rejected", [] {
        return rejected(
            R"({"version":1,"tasks":[{"method":"system.inspect","payload":{},"timeout_ms":300001}]})");
    });

    tests.emplace_back("payload must be object", [] {
        return rejected(
            R"({"version":1,"tasks":[{"method":"system.inspect","payload":["memory"],"timeout_ms":1000}]})");
    });

    tests.emplace_back("empty task list rejected", [] {
        return rejected(R"({"version":1,"tasks":[]})");
    });

    tests.emplace_back("task count limit enforced", [] {
        mcp::PlanValidatorConfig config;
        config.max_tasks = 1;
        config.allowed_methods = {{"system.inspect", {"system.read"}}};
        mcp::PlanValidator validator(config);
        const std::string json =
            R"({"version":1,"tasks":[{"method":"system.inspect","payload":{},"timeout_ms":1000},{"method":"system.inspect","payload":{},"timeout_ms":1000}]})";
        mcp::ValidatedPlan output;
        std::string error;
        return !validator.validate(json, output, error);
    });

    tests.emplace_back("plan byte limit enforced", [] {
        mcp::PlanValidatorConfig config;
        config.max_plan_bytes = 32;
        config.allowed_methods = {{"system.inspect", {"system.read"}}};
        mcp::PlanValidator validator(config);
        mcp::ValidatedPlan output;
        std::string error;
        return !validator.validate(
            R"({"version":1,"tasks":[]})", output, error);
    });

    tests.emplace_back("payload byte limit enforced", [] {
        mcp::PlanValidatorConfig config;
        config.max_payload_bytes = 8;
        config.allowed_methods = {{"system.inspect", {"system.read"}}};
        mcp::PlanValidator validator(config);
        const std::string json =
            R"({"version":1,"tasks":[{"method":"system.inspect","payload":{"target":"memory"},"timeout_ms":1000}]})";
        mcp::ValidatedPlan output;
        std::string error;
        return !validator.validate(json, output, error);
    });

    tests.emplace_back("capabilities come from trusted policy", [] {
        auto validator = make_validator();
        const std::string json =
            R"({"version":1,"tasks":[{"method":"system.configure","payload":{"mode":"safe"},"timeout_ms":5000}]})";
        mcp::ValidatedPlan output;
        std::string error;
        if (!validator.validate(json, output, error)) return false;
        return output.tasks.size() == 1 &&
               output.tasks[0].required_capabilities ==
                   std::vector<std::string>{"system.write"};
    });

    tests.emplace_back("failed validation leaves output unchanged", [] {
        auto validator = make_validator();
        mcp::ValidatedPlan output;
        output.version = 77;
        mcp::PlannedTask sentinel;
        sentinel.method = "sentinel";
        output.tasks.push_back(sentinel);

        std::string error;
        const bool accepted = validator.validate(
            R"({"version":1,"tasks":[{"method":"unknown.method","payload":{},"timeout_ms":1000}]})",
            output, error);

        return !accepted && output.version == 77 &&
               output.tasks.size() == 1 &&
               output.tasks[0].method == "sentinel";
    });

    tests.emplace_back("duplicate trusted methods rejected", [] {
        mcp::PlanValidatorConfig config;
        config.allowed_methods = {
            {"system.inspect", {"system.read"}},
            {"system.inspect", {"system.write"}}
        };
        mcp::PlanValidator validator(config);
        mcp::ValidatedPlan output;
        std::string error;
        return !validator.validate(
            R"({"version":1,"tasks":[{"method":"system.inspect","payload":{},"timeout_ms":1000}]})",
            output, error);
    });

    std::size_t passed = 0;
    for (const auto& test : tests) {
        bool ok = false;
        try {
            ok = test.second();
        } catch (...) {
            ok = false;
        }

        std::cout << (ok ? "[PASS] " : "[FAIL] ")
                  << test.first << '\n';
        if (ok) ++passed;
    }

    std::cout << passed << "/" << tests.size()
              << " tests passed\n";

    return passed == tests.size() ? 0 : 1;
}
