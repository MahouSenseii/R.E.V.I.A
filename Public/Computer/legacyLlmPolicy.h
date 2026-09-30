#pragma once

#include "LLM/responseTypes.h"
#include "Computer/computerPolicy.h"

#include <functional>
#include <string>

namespace revia::computer
{

// Baseline model policy uses the supplied planner and shared bounded observation.
class LegacyLlmComputerPolicy final : public IComputerPolicy
{
public:
    // Asked for one next-step decision, given the formatted context and the current
    // operation's token. The token must reach the request, not merely be checked after
    // it returns.
    using PlannerCall = std::function<responseOutput(const std::string&, std::stop_token)>;

    explicit LegacyLlmComputerPolicy(PlannerCall planner);

    [[nodiscard]] std::string Name() const override { return "legacy_llm"; }
    // The planner call is always installed by the session; a missing one is a wiring
    // mistake rather than a runtime condition, and it reports unavailable rather than
    // crashing on it.
    [[nodiscard]] bool IsAvailable() const override { return static_cast<bool>(plannerCall); }

    [[nodiscard]] ComputerDecision Decide(const ComputerTaskContext& context, std::stop_token stopToken) override;

private:
    PlannerCall plannerCall;
};

// Formats the shared context for the legacy planner without taking another observation.
[[nodiscard]] std::string FormatLegacyContext(const ComputerTaskContext& context);

} // namespace revia::computer
