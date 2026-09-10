#include "Policy/actionApproval.h"

#include "Actions/actionTypes.h"

#include <vector>

namespace revia::policy
{

bool ApprovalScope::Matches(const ApprovalScope& other) const
{
    // Every field, with no partial credit. An approval that matched loosely would be an
    // approval for a category, and the whole point of this is that it is an approval for
    // one thing.
    return taskId == other.taskId && operation == other.operation &&
        bindingId == other.bindingId && effects == other.effects &&
        parameterFingerprint == other.parameterFingerprint &&
        policyVersion == other.policyVersion;
}

std::string ApprovalRegistry::Grant(
    ApprovalScope scope,
    const std::chrono::steady_clock::time_point now,
    const std::chrono::milliseconds lifetime)
{
    ActionApproval approval;
    // Generated here. An id that could be supplied from outside would be a permission
    // written by whoever supplied it.
    approval.id = "approval-" + actions::NewActionId();
    approval.scope = std::move(scope);
    approval.createdAt = now;
    approval.expiresAt = now + lifetime;

    std::lock_guard lock(mutex);
    const std::string id = approval.id;
    approvals.emplace(id, std::move(approval));
    return id;
}

bool ApprovalRegistry::Consume(
    const std::string& approvalId,
    const ApprovalScope& against,
    const std::chrono::steady_clock::time_point now,
    std::string& outReason)
{
    std::lock_guard lock(mutex);
    const auto found = approvals.find(approvalId);
    if (found == approvals.end())
    {
        // Covers both a forged id and one that was already invalidated. The reason is
        // deliberately the same for both: telling a caller which of the two it was would
        // let it map the registry.
        outReason = "there is no approval by that name";
        return false;
    }

    ActionApproval& approval = found->second;
    if (approval.consumed)
    {
        outReason = "that approval has already been used";
        return false;
    }
    if (now >= approval.expiresAt)
    {
        outReason = "that approval expired";
        return false;
    }
    if (!approval.scope.Matches(against))
    {
        // The common case in practice: the recipient, the amount, the target or the
        // policy changed between approving and acting, so the thing being approved is no
        // longer the thing being done.
        outReason = "what is about to happen is not what was approved";
        return false;
    }

    approval.consumed = true;
    outReason.clear();
    return true;
}

void ApprovalRegistry::InvalidateTask(const std::string& taskId, const std::string&)
{
    std::lock_guard lock(mutex);
    for (auto& [id, approval] : approvals)
    {
        (void)id;
        if (approval.scope.taskId == taskId)
        {
            // Marked spent rather than erased, so a later attempt to use it reports
            // "already used" rather than "no such approval" -- the same answer a real
            // double-spend gets.
            approval.consumed = true;
        }
    }
}

void ApprovalRegistry::InvalidateAll(const std::string&)
{
    std::lock_guard lock(mutex);
    for (auto& [id, approval] : approvals)
    {
        (void)id;
        approval.consumed = true;
    }
}

void ApprovalRegistry::Prune(const std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock(mutex);
    std::vector<std::string> stale;
    for (const auto& [id, approval] : approvals)
    {
        if (approval.consumed || now >= approval.expiresAt)
        {
            stale.push_back(id);
        }
    }
    for (const std::string& id : stale)
    {
        approvals.erase(id);
    }
}

std::size_t ApprovalRegistry::OutstandingCount() const
{
    std::lock_guard lock(mutex);
    std::size_t outstanding = 0;
    for (const auto& [id, approval] : approvals)
    {
        (void)id;
        if (!approval.consumed) ++outstanding;
    }
    return outstanding;
}

} // namespace revia::policy
