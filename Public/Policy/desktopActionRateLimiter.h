#pragma once

#include "Actions/actionTypes.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <string>

namespace revia::policy
{

// Budgets admitted mutation, not evaluation or denied confirmation.
// UIA operations and synthesized input have separate process-local budgets.
class DesktopActionRateLimiter
{
public:
    enum class Scope
    {
        UiAutomation,
        DesktopControl
    };

    void Configure(int maxActionsPerMinute, int minimumIntervalMs, Scope scope = Scope::UiAutomation);
    [[nodiscard]] bool Admit(const actions::ActionRequest& request, std::chrono::steady_clock::time_point now, std::string& outReason);
    void Reset();

private:
    [[nodiscard]] bool Governs(actions::ActionType type) const;

    std::mutex mutex;
    std::deque<std::chrono::steady_clock::time_point> recentAdmissions;
    std::chrono::steady_clock::time_point lastAdmission{};
    Scope scope = Scope::UiAutomation;
    int maxPerMinute = 12;
    std::chrono::milliseconds minimumInterval{250};
};

} // namespace revia::policy
