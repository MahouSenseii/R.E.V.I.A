#pragma once

#include "Actions/IActionExecutor.h"
#include "Policy/desktopAuthorization.h"
#include "Policy/desktopInputGuard.h"

#include <memory>
#include <string>
#include <vector>

namespace revia::actions::windows
{

// In application scope, rechecks foreground ownership and window bounds before input.
// UIA-resolved vision targets use current element bounds, never planned coordinates.

// Splits UTF-16 into Unicode scalar values, keeping a surrogate pair together.
//
// Exposed because it is the difference between delivering one emoji and delivering two
// broken halves of one, and that is worth a test that does not need a desktop.
[[nodiscard]] std::vector<std::wstring> SplitScalars(const std::wstring& text);

class DesktopControlExecutor final : public IActionExecutor
{
public:
    DesktopControlExecutor(CapabilitySettings::DesktopControl settings, std::shared_ptr<policy::DesktopInputGuard> guard,
        std::shared_ptr<policy::DesktopApprovalGate> approvals = {});

    [[nodiscard]] bool Handles(ActionType type) const override;
    [[nodiscard]] ActionResult Execute(const ActionRequest& request, const PolicyDecision& decision) override;

private:
    CapabilitySettings::DesktopControl settings;
    std::shared_ptr<policy::DesktopInputGuard> guard;
    std::shared_ptr<policy::DesktopApprovalGate> approvals;
};

} // namespace revia::actions::windows
