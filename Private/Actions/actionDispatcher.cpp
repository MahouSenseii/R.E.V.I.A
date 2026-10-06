#include "Actions/actionDispatcher.h"

#include <stdexcept>
#include <algorithm>

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

void ActionDispatcher::Clear()
{
    executors.clear();
}

void ActionDispatcher::Unregister(const ActionType type)
{
    std::erase_if(executors, [type](const auto& executor) { return executor->Handles(type); });
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

    for (const auto& registered : executors)
    {
        // A live capability update can rebuild the registry from an effect callback.
        const auto executor = registered;
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
