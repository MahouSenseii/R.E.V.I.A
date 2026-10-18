#include "Workspace/virtualMachineExecutor.h"

#include "Workspace/projectShare.h"

#include <algorithm>

namespace revia::workspace
{
namespace
{
// Copies one approved host folder into the guest, batch by batch, publishing it only with
// the last batch. Each batch is its own guest request, bound to the same boot and rechecked
// against the turn's admission before it is sent.
actions::ActionResult ShareProject(VirtualMachineRuntime& runtime, const actions::ActionRequest& request,
    const actions::PolicyDecision& decision, actions::ActionResult result)
{
    ProjectShareLimits limits;
    limits.maxFileBytes = std::min(limits.maxFileBytes, decision.guestLimits.maxWriteBytes);
    const auto plan = PlanProjectShare(decision.canonicalSource, limits, request.executionStop);
    if (!plan.error.empty())
    {
        result.message = plan.error;
        return result;
    }
    GuestResult last;
    for (std::size_t index = 0; index < plan.batches.size(); ++index)
    {
        if (index > 0)
        {
            const auto refusal = request.beforeEffect(request.guest.expectedIdentity.environmentId);
            if (!refusal.empty())
            {
                result.message = refusal + " The project was not published in the guest.";
                return result;
            }
        }
        GuestRequest batch = request.guest;
        batch.files = plan.batches[index];
        batch.review.clear();
        if (index + 1 < plan.batches.size())
            batch.publish.clear();
        last = runtime.Execute(batch, decision.guestLimits, request.executionStop);
        result.attempted = true;
        if (!last.succeeded)
        {
            result.message = "The guest refused part of the project (" + (last.code.empty() ? std::string("no reason") : last.code) +
                             "); it was not published.";
            result.guest = std::move(last);
            return result;
        }
    }
    result.succeeded = true;
    result.message = "Shared " + std::to_string(plan.files) + " text files (" + std::to_string(plan.bytes) + " bytes) as guest project " +
                     request.guest.publish + ".";
    if (plan.skippedCount > 0)
        result.message += " Left out " + std::to_string(plan.skippedCount) + " files.";
    result.entries = plan.skipped;
    result.guest = std::move(last);
    return result;
}
}

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
    if (*operation == GuestOperation::ImportFiles)
        return ShareProject(*runtime, request, decision, std::move(result));
    auto receipt = runtime->Execute(request.guest, decision.guestLimits, request.executionStop);
    result.attempted = receipt.attempted;
    result.succeeded = receipt.succeeded;
    result.message = receipt.message;
    if (*operation == GuestOperation::ReadText && receipt.succeeded && !receipt.digest.empty())
        result.message += " SHA256: " + receipt.digest;
    result.content = *operation == GuestOperation::SearchText ? receipt.standardOutput : receipt.content;
    result.entries = receipt.entries;
    result.guest = std::move(receipt);
    return result;
}
}
