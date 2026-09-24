#pragma once

#include "Actions/actionTypes.h"

namespace revia::actions
{

class IActionExecutor
{
public:
    virtual ~IActionExecutor() = default;

    [[nodiscard]] virtual bool Handles(ActionType type) const = 0;
    [[nodiscard]] virtual ActionResult Execute(
        const ActionRequest& request,
        const PolicyDecision& decision) = 0;
};

} // namespace revia::actions
