#pragma once

#include "Actions/actionTypes.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <string>

namespace revia::policy
{

// Process-local execution budget for mutable UI Automation actions. It records admission,
// not planning/evaluation, so a denied confirmation does not consume the user's budget.
class DesktopActionRateLimiter
{
public:
    void Configure(int maxActionsPerMinute, int minimumIntervalMs);
    [[nodiscard]] bool Admit(
        const actions::ActionRequest& request,
        std::chrono::steady_clock::time_point now,
        std::string& outReason);
    void Reset();

private:
    std::mutex mutex;
    std::deque<std::chrono::steady_clock::time_point> recentAdmissions;
    std::chrono::steady_clock::time_point lastAdmission{};
    int maxPerMinute = 12;
    std::chrono::milliseconds minimumInterval{250};
};

} // namespace revia::policy
