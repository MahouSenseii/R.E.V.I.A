#pragma once

#include "Actions/IActionExecutor.h"

#include <memory>
#include <vector>

namespace revia::actions
{

class ActionDispatcher
{
public:
    void Register(std::unique_ptr<IActionExecutor> executor);
    void Clear();

    [[nodiscard]] ActionResult Dispatch(const ActionRequest& request,const PolicyDecision& decision,bool confirmationGranted = false);

private:
    std::vector<std::unique_ptr<IActionExecutor>> executors;
};

} // namespace revia::actions
