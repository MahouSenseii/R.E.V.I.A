#include "Policy/desktopActionRateLimiter.h"

#include <algorithm>

namespace revia::policy
{

void DesktopActionRateLimiter::Configure(
    const int maxActionsPerMinute,
    const int minimumIntervalMs,
    const Scope inputScope)
{
    std::lock_guard lock(mutex);
    scope = inputScope;
    maxPerMinute = std::max(maxActionsPerMinute, 1);
    minimumInterval = std::chrono::milliseconds(std::max(minimumIntervalMs, 0));
    recentAdmissions.clear();
    lastAdmission = {};
}

bool DesktopActionRateLimiter::Governs(const actions::ActionType type) const
{
    if (scope == Scope::DesktopControl)
    {
        return actions::IsDesktopControlAction(type);
    }
    return type == actions::ActionType::FocusWindow ||
        type == actions::ActionType::SetControlText ||
        type == actions::ActionType::InvokeControl;
}

bool DesktopActionRateLimiter::Admit(
    const actions::ActionRequest& request,
    const std::chrono::steady_clock::time_point now,
    std::string& outReason)
{
    if (request.dryRun ||
        actions::RiskForAction(request.type) == actions::RiskLevel::ReadOnly ||
        !Governs(request.type))
    {
        outReason.clear();
        return true;
    }

    std::lock_guard lock(mutex);
    const auto windowStart = now - std::chrono::minutes(1);
    while (!recentAdmissions.empty() && recentAdmissions.front() <= windowStart)
    {
        recentAdmissions.pop_front();
    }
    const bool desktopControl = scope == Scope::DesktopControl;
    if (lastAdmission != std::chrono::steady_clock::time_point{} &&
        now - lastAdmission < minimumInterval)
    {
        outReason = desktopControl
            ? "Desktop control rate limit: input may not be synthesized this quickly."
            : "Desktop action rate limit: controls may not be changed this quickly.";
        return false;
    }
    if (static_cast<int>(recentAdmissions.size()) >= maxPerMinute)
    {
        outReason = desktopControl
            ? "Desktop control rate limit: the per-minute input budget is exhausted."
            : "Desktop action rate limit: the per-minute action budget is exhausted.";
        return false;
    }
    recentAdmissions.push_back(now);
    lastAdmission = now;
    outReason.clear();
    return true;
}

void DesktopActionRateLimiter::Reset()
{
    std::lock_guard lock(mutex);
    recentAdmissions.clear();
    lastAdmission = {};
}

} // namespace revia::policy
