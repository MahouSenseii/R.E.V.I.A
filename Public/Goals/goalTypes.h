#pragma once

#include "Actions/actionTypes.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace revia::goals
{

// Lifecycle of a whole goal. Persisted, so a restart can pick it back up.
enum class GoalStatus
{
    Planned,
    Running,
    Blocked,
    Succeeded,
    Failed,
    Cancelled,
    Exhausted
};

// Lifecycle of one step inside the act / observe / verify cycle.
enum class StepStatus
{
    Pending,
    Acting,
    Verifying,
    Succeeded,
    Failed,
    Skipped
};

// Why the runner stopped. Recorded so a bad run is diagnosable afterwards
// instead of only visible in its consequences.
enum class StopReason
{
    None,
    Completed,
    VerificationFailed,
    PolicyBlocked,
    BudgetActions,
    BudgetDuration,
    BudgetRetries,
    BudgetTokens,
    Cancelled,
    InvalidPlan,
    StoreError
};

// Hard ceilings. The runner stops when any single one is reached; it never
// negotiates a budget upward mid-run.
struct GoalBudget
{
    std::uint32_t maxActions = 20;
    std::uint32_t maxRetriesPerStep = 2;
    std::uint32_t maxTotalRetries = 6;
    std::uint32_t maxTokens = 8192;
    std::uint64_t maxDurationMs = 120000;
};

// What has actually been spent. Compared against GoalBudget after every step.
struct GoalSpend
{
    std::uint32_t actions = 0;
    std::uint32_t retries = 0;
    std::uint32_t tokens = 0;
    std::uint64_t elapsedMs = 0;
};

// One attempt at one step: what was tried, and what came back.
// actionId and checkActionId are the ActionRequest ids, so an attempt can be
// joined against the existing JSONL audit log.
struct StepAttempt
{
    std::uint32_t attempt = 0;
    std::string actionId;
    std::string checkActionId;
    actions::PolicyVerdict verdict = actions::PolicyVerdict::Blocked;
    bool executed = false;
    bool verified = false;
    std::string observation;
    std::string failure;
    std::chrono::system_clock::time_point occurredAt = std::chrono::system_clock::now();
};

// A unit of work: perform one typed action, then prove it happened.
// `check` is a read-only action, so verification costs no extra authority.
struct GoalStep
{
    std::string id;
    std::uint32_t ordinal = 0;
    std::string description;
    StepStatus status = StepStatus::Pending;

    actions::ActionRequest action;
    actions::ActionRequest check;
    std::string expected;

    std::vector<StepAttempt> attempts;
};

struct Goal
{
    std::string id;
    std::string title;
    GoalStatus status = GoalStatus::Planned;
    StopReason stopReason = StopReason::None;

    std::vector<GoalStep> steps;
    std::uint32_t currentStep = 0;

    GoalBudget budget;
    GoalSpend spend;

    // Per-goal capability scope, narrower than the global profile. This feeds a
    // CapabilityPolicy directly, so a goal can never widen its own authority.
    actions::CapabilitySettings scope;

    std::chrono::system_clock::time_point createdAt = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point updatedAt = std::chrono::system_clock::now();
};

[[nodiscard]] std::string ToString(GoalStatus value);
[[nodiscard]] std::string ToString(StepStatus value);
[[nodiscard]] std::string ToString(StopReason value);
[[nodiscard]] GoalStatus GoalStatusFromString(const std::string& value);
[[nodiscard]] StepStatus StepStatusFromString(const std::string& value);
[[nodiscard]] StopReason StopReasonFromString(const std::string& value);
[[nodiscard]] bool IsTerminal(GoalStatus value);
[[nodiscard]] std::string NewGoalId();
[[nodiscard]] std::string NewStepId();

// Derives a goal's capability scope from the configured profile settings. The result is
// never wider than the input: approved roots and applications are carried across
// unchanged, mode is forced to ApprovedScope, root creation is refused, and the
// auto-approval ceiling is capped at ReversibleWrite. A goal must not be able to grant
// itself authority the interactive path does not already have, and a plan the model
// authored must not be able to name its own scope at all.
[[nodiscard]] actions::CapabilitySettings NarrowScopeForGoal(
    actions::CapabilitySettings configured);

} // namespace revia::goals
