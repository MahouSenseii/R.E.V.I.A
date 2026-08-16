#pragma once

#include "Actions/IActionExecutor.h"

namespace revia::actions::windows
{

class WindowsAutomationExecutor final : public IActionExecutor
{
public:
    [[nodiscard]] bool Handles(ActionType type) const override;
    [[nodiscard]] ActionResult Execute(
        const ActionRequest& request,
        const PolicyDecision& decision) override;
};

} // namespace revia::actions::windows
