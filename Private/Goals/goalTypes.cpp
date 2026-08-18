#include "Goals/goalTypes.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <sstream>

namespace revia::goals
{

namespace
{

// Mirrors the normalisation used by actions::ActionTypeFromString so that
// "Verification Failed", "verification-failed" and "verification_failed" all
// round-trip to the same enum.
std::string NormalizeName(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
    {
        if (c == '-' || c == ' ')
        {
            return '_';
        }
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string MakeId(const char* prefix)
{
    static std::atomic<std::uint64_t> counter{1};
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream stream;
    stream << prefix << '-' << ticks << '-' << counter.fetch_add(1, std::memory_order_relaxed);
    return stream.str();
}

} // namespace

std::string ToString(const GoalStatus value)
{
    switch (value)
    {
        case GoalStatus::Planned: return "planned";
        case GoalStatus::Running: return "running";
        case GoalStatus::Blocked: return "blocked";
        case GoalStatus::Succeeded: return "succeeded";
        case GoalStatus::Failed: return "failed";
        case GoalStatus::Cancelled: return "cancelled";
        case GoalStatus::Exhausted: return "exhausted";
    }
    return "planned";
}

std::string ToString(const StepStatus value)
{
    switch (value)
    {
        case StepStatus::Pending: return "pending";
        case StepStatus::Acting: return "acting";
        case StepStatus::Verifying: return "verifying";
        case StepStatus::Succeeded: return "succeeded";
        case StepStatus::Failed: return "failed";
        case StepStatus::Skipped: return "skipped";
    }
    return "pending";
}

std::string ToString(const StopReason value)
{
    switch (value)
    {
        case StopReason::None: return "none";
        case StopReason::Completed: return "completed";
        case StopReason::VerificationFailed: return "verification_failed";
        case StopReason::PolicyBlocked: return "policy_blocked";
        case StopReason::BudgetActions: return "budget_actions";
        case StopReason::BudgetDuration: return "budget_duration";
        case StopReason::BudgetRetries: return "budget_retries";
        case StopReason::BudgetTokens: return "budget_tokens";
        case StopReason::Cancelled: return "cancelled";
        case StopReason::StoreError: return "store_error";
    }
    return "none";
}

// Unknown text fails closed: an unreadable status row is treated as Blocked
// rather than Running, so a corrupt store can never resume execution.
GoalStatus GoalStatusFromString(const std::string& value)
{
    const std::string name = NormalizeName(value);
    if (name == "planned") return GoalStatus::Planned;
    if (name == "running") return GoalStatus::Running;
    if (name == "blocked") return GoalStatus::Blocked;
    if (name == "succeeded") return GoalStatus::Succeeded;
    if (name == "failed") return GoalStatus::Failed;
    if (name == "cancelled" || name == "canceled") return GoalStatus::Cancelled;
    if (name == "exhausted") return GoalStatus::Exhausted;
    return GoalStatus::Blocked;
}

StepStatus StepStatusFromString(const std::string& value)
{
    const std::string name = NormalizeName(value);
    if (name == "pending") return StepStatus::Pending;
    if (name == "acting") return StepStatus::Acting;
    if (name == "verifying") return StepStatus::Verifying;
    if (name == "succeeded") return StepStatus::Succeeded;
    if (name == "failed") return StepStatus::Failed;
    if (name == "skipped") return StepStatus::Skipped;
    return StepStatus::Failed;
}

StopReason StopReasonFromString(const std::string& value)
{
    const std::string name = NormalizeName(value);
    if (name == "completed") return StopReason::Completed;
    if (name == "verification_failed") return StopReason::VerificationFailed;
    if (name == "policy_blocked") return StopReason::PolicyBlocked;
    if (name == "budget_actions") return StopReason::BudgetActions;
    if (name == "budget_duration") return StopReason::BudgetDuration;
    if (name == "budget_retries") return StopReason::BudgetRetries;
    if (name == "budget_tokens") return StopReason::BudgetTokens;
    if (name == "cancelled" || name == "canceled") return StopReason::Cancelled;
    if (name == "store_error") return StopReason::StoreError;
    return StopReason::None;
}

// Blocked is deliberately not terminal: a goal waiting on confirmation is
// still resumable. Everything else here is final.
bool IsTerminal(const GoalStatus value)
{
    switch (value)
    {
        case GoalStatus::Succeeded:
        case GoalStatus::Failed:
        case GoalStatus::Cancelled:
        case GoalStatus::Exhausted:
            return true;
        case GoalStatus::Planned:
        case GoalStatus::Running:
        case GoalStatus::Blocked:
            return false;
    }
    return false;
}

std::string NewGoalId()
{
    return MakeId("goal");
}

std::string NewStepId()
{
    return MakeId("step");
}

} // namespace revia::goals
