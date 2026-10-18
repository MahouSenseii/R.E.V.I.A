#pragma once

#include "Actions/IActionExecutor.h"
#include "Workspace/virtualMachineRuntime.h"

#include <memory>

namespace revia::workspace
{
class VirtualMachineExecutor final : public actions::IActionExecutor
{
  public:
    explicit VirtualMachineExecutor(std::shared_ptr<VirtualMachineRuntime> runtime) : runtime(std::move(runtime)) {}
    [[nodiscard]] bool Handles(actions::ActionType type) const override;
    [[nodiscard]] actions::ActionResult Execute(const actions::ActionRequest& request, const actions::PolicyDecision& decision) override;

  private:
    std::shared_ptr<VirtualMachineRuntime> runtime;
};
}
