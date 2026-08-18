#pragma once

#include "Actions/actionRuntime.h"
#include "Goals/goalStore.h"
#include "Goals/goalTypes.h"

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>

namespace revia::goals
{

// Published after every transition so the desktop activity feed can follow a
// goal without polling the store.
struct GoalProgress
{
    std::string goalId;
    std::string stepId;
    std::uint32_t ordinal = 0;
    StepStatus stepStatus = StepStatus::Pending;
    GoalStatus goalStatus = GoalStatus::Planned;
    std::string message;
};

// Bounded plan / act / observe / verify loop over the existing typed actions.
//
// The runner adds no execution authority of its own. Every action goes through
// ActionRuntime::ExecuteScoped, which is the same dispatcher and the same audit
// logger the interactive path already uses. What it adds is the requirement
// that a step prove it happened before the goal is allowed to move on.
class GoalRunner
{
public:
    using ProgressHandler = std::function<void(const GoalProgress&)>;
    using ConfirmationHandler = std::function<bool(
        const actions::ActionRequest&,
        const actions::PolicyDecision&)>;

    GoalRunner(actions::ActionRuntime& runtime, const GoalStore& store);

    GoalRunner(const GoalRunner&) = delete;
    GoalRunner& operator=(const GoalRunner&) = delete;

    void SetProgressHandler(ProgressHandler handler);
    void SetConfirmationHandler(ConfirmationHandler handler);

    // Validates the plan, then runs it. Returns the goal in its final state;
    // that same state has already been written to the store.
    [[nodiscard]] Goal Run(Goal goal, std::stop_token stopToken = {});

    // Reloads a goal an earlier process left unfinished and continues it.
    [[nodiscard]] Goal Resume(const std::string& goalId, std::stop_token stopToken = {});

    // A plan is rejected before anything executes when a step has no
    // verification action, when that action is not read-only, or when the step
    // does not say what success looks like.
    [[nodiscard]] static bool Validate(const Goal& goal, std::string& outError);

private:
    bool RunStep(
        Goal& goal,
        GoalStep& step,
        const policy::CapabilityPolicy& scopedPolicy,
        std::stop_token stopToken);
    [[nodiscard]] static StopReason CheckBudget(const Goal& goal);
    void Publish(const Goal& goal, const GoalStep& step, const std::string& message) const;
    bool Persist(Goal& goal) const;

    actions::ActionRuntime& actionRuntime;
    const GoalStore& goalStore;
    ProgressHandler progressHandler;
    ConfirmationHandler confirmationHandler;
};

} // namespace revia::goals
