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

// One answer to "what should she do next?", produced fresh from what the machine looks
// like right now rather than read off a plan written before any of it was seen.
//
// Three outcomes, deliberately distinct. A step to take; nothing left to take because
// the goal is met; or no usable answer at all. Collapsing the last two would make
// "finished" and "stuck" the same record, and they are opposite outcomes.
struct NextStep
{
    bool hasStep = false;
    bool finished = false;
    GoalStep step;
    std::string reason;
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
    // Consulted once per iteration by Operate. It receives the goal with every
    // attempt so far already recorded on it, which is the whole history the decision
    // gets. Observing the machine is the provider's job, not the runner's: keeping the
    // observation on that side is what stops Goals from depending on Windows.
    using StepProvider = std::function<NextStep(const Goal&, std::uint32_t iteration)>;

    GoalRunner(actions::ActionRuntime& runtime, const GoalStore& store);

    GoalRunner(const GoalRunner&) = delete;
    GoalRunner& operator=(const GoalRunner&) = delete;

    void SetProgressHandler(ProgressHandler handler);
    void SetConfirmationHandler(ConfirmationHandler handler);
    void SetStepProvider(StepProvider provider);

    // Validates the plan, then runs it. Returns the goal in its final state;
    // that same state has already been written to the store.
    [[nodiscard]] Goal Run(Goal goal, std::stop_token stopToken = {});

    // The iterative form: observe, decide one action, do it, prove it happened, look
    // again. `goal.steps` starts empty and is appended to as the run discovers what the
    // work actually turned out to be, so the record afterwards is what she really did
    // rather than what someone guessed beforehand.
    //
    // A separate entry point rather than a mode on Run, because the planned path works
    // and there is no reason for this to be able to break it. Everything underneath is
    // shared: the same budgets, the same scoped policy, the same RunStep, the same
    // audit log, the same store.
    //
    // Requires a step provider. Without one it refuses rather than running zero steps
    // and reporting success.
    [[nodiscard]] Goal Operate(Goal goal, std::stop_token stopToken = {});

    // Reloads a goal an earlier process left unfinished and continues it.
    [[nodiscard]] Goal Resume(const std::string& goalId, std::stop_token stopToken = {});

    // A plan is rejected before anything executes when a step has no
    // verification action, when that action is not read-only, or when the step
    // does not say what success looks like.
    [[nodiscard]] static bool Validate(const Goal& goal, std::string& outError);
    // The same rules applied to one step. Operate checks every step it is handed with
    // this, so a step invented mid-run faces exactly the checks a planned one does.
    [[nodiscard]] static bool ValidateStep(const GoalStep& step, std::string& outError);

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
    StepProvider stepProvider;
};

} // namespace revia::goals
