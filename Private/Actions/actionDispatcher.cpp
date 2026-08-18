#include "Actions/actionDispatcher.h"

#include <stdexcept>

namespace revia::actions
{

void ActionDispatcher::Register(std::unique_ptr<IActionExecutor> executor)
{
    if (!executor)
    {
        throw std::invalid_argument("ActionDispatcher cannot register a null executor.");
    }
    executors.push_back(std::move(executor));
}

ActionResult ActionDispatcher::Dispatch(const ActionRequest& request,const PolicyDecision& decision,bool confirmationGranted)
{
    ActionResult result;
    result.dryRun = request.dryRun;

    if (decision.verdict == PolicyVerdict::Blocked)
    {
        result.message = "Action blocked by policy: " + decision.reason;
        return result;
    }

    if (decision.verdict == PolicyVerdict::RequiresConfirmation && !confirmationGranted)
    {
        result.message = "Action was not executed because confirmation was not granted.";
        return result;
    }

    for (const auto& executor : executors)
    {
        if (executor->Handles(request.type))
        {
            try
            {
                return executor->Execute(request, decision);
            }
            catch (const std::exception& error)
            {
                result.attempted = true;
                result.message = std::string("Executor failed: ") + error.what();
                return result;
            }
            catch (...)
            {
                result.attempted = true;
                result.message = "Executor failed with an unknown error.";
                return result;
            }
        }
    }

    result.message = "No executor is registered for action type: " + ToString(request.type);
    return result;
}

} // namespace revia::actions
