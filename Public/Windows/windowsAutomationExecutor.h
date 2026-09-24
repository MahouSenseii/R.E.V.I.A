#pragma once

#include "Actions/IActionExecutor.h"
#include "Policy/desktopAuthorization.h"

#include <memory>

namespace revia::actions::windows
{

// Asking a control to act on itself is not gentler than clicking it.
//
// Invoke and SetValue reach exactly the consequences a mouse reaches, so this executor
// carries the same desktop-control settings and asks the same shared authorizer. Before
// that, clicking Send was gated and invoking the same button through UI Automation was
// not, which made the boundary a matter of which route was chosen.
class WindowsAutomationExecutor final : public IActionExecutor
{
public:
    explicit WindowsAutomationExecutor(
        CapabilitySettings::DesktopControl settings = {},
        std::shared_ptr<policy::DesktopApprovalGate> approvals = {});

    [[nodiscard]] bool Handles(ActionType type) const override;
    [[nodiscard]] ActionResult Execute(
        const ActionRequest& request,
        const PolicyDecision& decision) override;

private:
    CapabilitySettings::DesktopControl settings;
    std::shared_ptr<policy::DesktopApprovalGate> approvals;
};

} // namespace revia::actions::windows
