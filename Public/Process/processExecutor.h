#pragma once

#include "Actions/IActionExecutor.h"

namespace revia::process
{

// Each execution owns an isolated child job, pipe handles and their bounded receipt.
// The working directory selects context; this is not filesystem or account isolation.
class ProcessExecutor final : public actions::IActionExecutor
{
  public:
    [[nodiscard]] bool Handles(actions::ActionType type) const override;
    [[nodiscard]] actions::ActionResult Execute(const actions::ActionRequest& request, const actions::PolicyDecision& decision) override;
};

} // namespace revia::process
