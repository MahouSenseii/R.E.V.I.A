#pragma once

#include "Actions/IActionExecutor.h"
#include "Policy/desktopInputGuard.h"

#include <memory>
#include <string>
#include <vector>

namespace revia::actions::windows
{

// Revia's hands.
//
// Everything here is indistinguishable from the person at the keyboard once it leaves
// the process, so containment happens before it leaves: policy has already checked the
// capability switches and the approved application list, and this executor additionally
// refuses to synthesize anything unless the foreground window still belongs to that
// approved application, and refuses to aim at a point outside that window.
//
// A vision-resolved click re-finds its element and uses the element's current bounds.
// The coordinate captured when the plan was made is never the coordinate clicked.

// Splits UTF-16 into Unicode scalar values, keeping a surrogate pair together.
//
// Exposed because it is the difference between delivering one emoji and delivering two
// broken halves of one, and that is worth a test that does not need a desktop.
[[nodiscard]] std::vector<std::wstring> SplitScalars(const std::wstring& text);

class DesktopControlExecutor final : public IActionExecutor
{
public:
    DesktopControlExecutor(
        CapabilitySettings::DesktopControl settings,
        std::shared_ptr<policy::DesktopInputGuard> guard);

    [[nodiscard]] bool Handles(ActionType type) const override;
    [[nodiscard]] ActionResult Execute(
        const ActionRequest& request,
        const PolicyDecision& decision) override;

private:
    CapabilitySettings::DesktopControl settings;
    std::shared_ptr<policy::DesktopInputGuard> guard;
};

} // namespace revia::actions::windows
