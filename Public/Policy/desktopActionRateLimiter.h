#pragma once

#include "Actions/actionTypes.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <string>

namespace revia::policy
{

// Process-local execution budget for mutable desktop work. It records admission,
// not planning/evaluation, so a denied confirmation does not consume the user's budget.
//
// UI Automation and synthesized input are budgeted separately because they cost
// different things: asking a control to invoke itself is bounded by the control, while
// a stream of keystrokes and clicks is bounded only by how fast they can be sent.
class DesktopActionRateLimiter
{
public:
    enum class Scope
    {
        UiAutomation,
        DesktopControl
    };

    void Configure(int maxActionsPerMinute, int minimumIntervalMs, Scope scope = Scope::UiAutomation);
    [[nodiscard]] bool Admit(
        const actions::ActionRequest& request,
        std::chrono::steady_clock::time_point now,
        std::string& outReason);
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
