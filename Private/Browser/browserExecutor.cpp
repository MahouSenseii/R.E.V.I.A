#include "Browser/browserExecutor.h"

namespace revia::browser
{

actions::ActionResult BrowserExecutor::Execute(const actions::ActionRequest& request, const actions::PolicyDecision& decision)
{
    if (session)
        return session->Execute(request, decision.browser);
    actions::ActionResult result;
    result.message = "No host-owned interactive browser session is bound.";
    return result;
}

} // namespace revia::browser
