#pragma once

#include "Actions/IActionExecutor.h"
#include "Policy/desktopAuthorization.h"

#include <memory>

namespace revia::actions::windows
{

// Invoke and SetValue use the shared desktop authorizer and consequence settings.
class WindowsAutomationExecutor final : public IActionExecutor
{
public:
    explicit WindowsAutomationExecutor(CapabilitySettings::DesktopControl settings = {},
        std::shared_ptr<policy::DesktopApprovalGate> approvals = {});

    [[nodiscard]] bool Handles(ActionType type) const override;
    [[nodiscard]] ActionResult Execute(const ActionRequest& request, const PolicyDecision& decision) override;

private:
    CapabilitySettings::DesktopControl settings;
    std::shared_ptr<policy::DesktopApprovalGate> approvals;
};

} // namespace revia::actions::windows
