#pragma once

#include "Actions/actionRuntime.h"
#include "Goals/goalStore.h"
#include "Goals/goalTypes.h"
#include "Runtime/runtimeStamp.h"

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

// Distinct results: take a step, report completion, or no usable decision.
// Undecided must never be recorded as finished.
struct NextStep
{
    bool hasStep = false;
    bool finished = false;
    GoalStep step;
    std::string reason;
    // What producing this answer cost, as the backend reported it. Zero with
    // `costReported` false means it would not say, which is not the same as free --
    // see GoalSpend. The provider fills these because the runner never talks to a
    // model and has no other way to know.
    std::uint32_t tokens = 0;
    bool costReported = false;
    // Set when the answer is "this needs something from the person", which is neither
    // a step nor a finish nor being stuck. Only meaningful with `hasStep` false.
    bool needsInput = false;
};

// Bounded plan/act/observe/verify loop through ActionRuntime::ExecuteScoped.
// Grants no authority; each step requires evidence before advancing.
class GoalRunner
{
  public:
    using ProgressHandler = std::function<void(const GoalProgress&)>;
    using ConfirmationHandler = std::function<actions::ConfirmationChoice(const actions::ActionRequest&, const actions::PolicyDecision&)>;
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
    // Drops any standing yes. Called at the start of every run, so an approval can never
    // leak from one goal into the next.
    void ClearStandingApproval();
    // Consumed by the next run only; never stored or used to widen permissions.
    // Covers only the shown risk ceiling; escalation and deletion still require confirmation.
    void SeedStandingApproval(actions::RiskLevel ceiling, bool refuseEscalation = false);
    void SetStepProvider(StepProvider provider);
    void SetExecutionStamp(runtime::RuntimeStamp stamp);

    // Validates the plan, then runs it. Returns the goal in its final state;
    // that same state has already been written to the store.
    [[nodiscard]] Goal Run(Goal goal, std::stop_token stopToken = {});

    // Observes and appends one verified step at a time using the shared run pipeline.
    // Refuses without a step provider rather than reporting an empty run as success.
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
    actions::ActionOutcome DispatchScoped(
        const actions::ActionRequest& request, const policy::CapabilityPolicy& policy, bool confirmed, std::stop_token stopToken);
    bool RunStep(Goal& goal, GoalStep& step, const policy::CapabilityPolicy& scopedPolicy, std::stop_token stopToken);
    [[nodiscard]] static StopReason CheckBudget(const Goal& goal);
    void Publish(const Goal& goal, const GoalStep& step, const std::string& message) const;
    bool Persist(Goal& goal) const;

    actions::ActionRuntime& actionRuntime;
    const GoalStore& goalStore;
    ProgressHandler progressHandler;
    ConfirmationHandler confirmationHandler;
    // A standing yes for the current run. Reset at the start of every Run and Operate,
    // never persisted, and never consulted for anything riskier than what was shown.
    bool blanketApproval = false;
    bool refuseAdditionalApproval = false;
    actions::RiskLevel blanketRiskCeiling = actions::RiskLevel::ReadOnly;
    // Set before a run starts, consumed by it. Separate from the live flag so that
    // starting a run still clears whatever the previous one left behind.
    bool seededApproval = false;
    bool seededRefuseEscalation = false;
    actions::RiskLevel seededCeiling = actions::RiskLevel::ReadOnly;
    StepProvider stepProvider;
    runtime::RuntimeStamp executionStamp;
};

} // namespace revia::goals
