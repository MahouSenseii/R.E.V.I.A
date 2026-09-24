#pragma once

#include "Computer/computerPolicy.h"
#include "Library/structLibrary.h"

#include <functional>
#include <string>

namespace revia::computer
{

// The existing decision path, behind the policy interface and otherwise untouched.
//
// This is the baseline every other provider is measured against, so it must keep
// producing exactly what it produced before the interface existed. The prompt it
// builds is the same prompt, assembled by the same code, from the same bounded
// observation; the only thing that changed is where that observation was taken.
//
// It owns no model and no router. The call is supplied, so Computer depends on neither
// Core nor LLM, and a test can drive the whole policy with a fixture answer and no
// backend at all.
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

    [[nodiscard]] ComputerDecision Decide(
        const ComputerTaskContext& context,
        std::stop_token stopToken) override;

private:
    PlannerCall plannerCall;
};

// The prompt the legacy path sends, built from a context instead of from a live look.
//
// Exposed because it is the compatibility formatter the design requires: it is the one
// place that knows the shape the current model expects, and a test needs to be able to
// call it with a synthetic observation and no desktop.
[[nodiscard]] std::string FormatLegacyContext(const ComputerTaskContext& context);

} // namespace revia::computer
