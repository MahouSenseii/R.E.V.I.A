#pragma once

#include "Actions/IActionExecutor.h"
#include "Skills/mcpRegistry.h"

#include <memory>

namespace revia::skills
{

// Calls an MCP tool the policy already admitted.
//
// The request's value is "<server>/<tool>" and its arguments the JSON object the
// planner filled in. By the time this runs, the policy has looked the tool up in the
// registry, taken its pinned risk, and asked for confirmation when that risk
// required it; the audit record has the intent. What comes back is text the tool
// returned, which is reference data to her like any other result.
class McpToolExecutor final : public actions::IActionExecutor
{
public:
    explicit McpToolExecutor(std::shared_ptr<McpRegistry> registry);
    [[nodiscard]] bool Handles(actions::ActionType type) const override;
    [[nodiscard]] actions::ActionResult Execute(
        const actions::ActionRequest& request,
        const actions::PolicyDecision& decision) override;

    // "<server>/<tool>" into its parts; false when it is not that shape.
    [[nodiscard]] static bool SplitToolReference(
        const std::string& value, std::string& outServer, std::string& outTool);

private:
    std::shared_ptr<McpRegistry> registry;
};

} // namespace revia::skills
