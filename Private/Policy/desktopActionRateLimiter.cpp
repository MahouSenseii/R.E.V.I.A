#include "Policy/desktopActionRateLimiter.h"

#include <algorithm>

namespace revia::policy
{

void DesktopActionRateLimiter::Configure(
    const int maxActionsPerMinute,
    const int minimumIntervalMs)
{
    std::lock_guard lock(mutex);
    maxPerMinute = std::max(maxActionsPerMinute, 1);
    minimumInterval = std::chrono::milliseconds(std::max(minimumIntervalMs, 0));
    recentAdmissions.clear();
    lastAdmission = {};
}

bool DesktopActionRateLimiter::Admit(
    const actions::ActionRequest& request,
    const std::chrono::steady_clock::time_point now,
    std::string& outReason)
{
    if (request.dryRun ||
        actions::RiskForAction(request.type) == actions::RiskLevel::ReadOnly ||
        (request.type != actions::ActionType::FocusWindow &&
         request.type != actions::ActionType::SetControlText &&
         request.type != actions::ActionType::InvokeControl))
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
    if (lastAdmission != std::chrono::steady_clock::time_point{} &&
        now - lastAdmission < minimumInterval)
    {
        outReason = "Desktop action rate limit: controls may not be changed this quickly.";
        return false;
    }
    if (static_cast<int>(recentAdmissions.size()) >= maxPerMinute)
    {
        outReason = "Desktop action rate limit: the per-minute action budget is exhausted.";
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
