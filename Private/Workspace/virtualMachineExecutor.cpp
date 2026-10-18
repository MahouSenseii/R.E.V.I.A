#include "Workspace/virtualMachineExecutor.h"

namespace revia::workspace
{
bool VirtualMachineExecutor::Handles(const actions::ActionType type) const
{
    return actions::IsGuestAction(type);
}

actions::ActionResult VirtualMachineExecutor::Execute(const actions::ActionRequest& request, const actions::PolicyDecision& decision)
{
    actions::ActionResult result;
    result.dryRun = request.dryRun;
    result.backend = "ubuntu_vm";
    const auto operation = actions::GuestOperationForAction(request.type);
    if (!runtime || !operation || *operation != request.guest.operation || !request.beforeEffect ||
        decision.verdict == actions::PolicyVerdict::Blocked)
    {
        result.message = "The guest action requires a matching host-owned executor and admitted submission guard.";
        return result;
    }
    if (request.executionStop.stop_requested())
    {
        result.message = "The guest action was cancelled before submission.";
        return result;
    }
    if (request.beforeEffect)
    {
        const auto refusal = request.beforeEffect(request.guest.expectedIdentity.environmentId);
        if (!refusal.empty())
        {
            result.message = refusal;
            return result;
        }
    }
    if (request.dryRun)
    {
        result.succeeded = true;
        result.message = "Dry run: the typed guest action was admitted without contacting the VM.";
        return result;
    }
    auto receipt = runtime->Execute(request.guest, decision.guestLimits, request.executionStop);
    result.attempted = receipt.attempted;
    result.succeeded = receipt.succeeded;
    result.message = receipt.message;
    if (*operation == GuestOperation::ReadText && receipt.succeeded && !receipt.digest.empty())
        result.message += " SHA256: " + receipt.digest;
    result.content = receipt.content;
    result.entries = receipt.entries;
    result.guest = std::move(receipt);
    return result;
}
}
