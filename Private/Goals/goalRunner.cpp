#include "Goals/goalRunner.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace revia::goals
{

namespace
{

constexpr std::size_t MaxObservationCharacters = 1024;

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

// The observation is stored, not just its verdict, so a wrong verification is
// diagnosable afterwards rather than only visible in its consequences.
std::string SummarizeResult(const actions::ActionResult& result)
{
    std::string summary = result.message;
    const auto append = [&summary](const std::string& value)
    {
        if (value.empty())
        {
            return;
        }
        if (!summary.empty())
        {
            summary += " | ";
        }
        summary += value;
    };

    append(result.content);
    for (const std::string& entry : result.entries)
    {
        append(entry);
    }

    if (summary.size() > MaxObservationCharacters)
    {
        summary.resize(MaxObservationCharacters);
        summary += "...";
    }
    return summary;
}

// Success is observed, never assumed. The check has to run, succeed, and
// contain the text the step said it was looking for.
bool Observed(const actions::ActionResult& result, const std::string& expected)
{
    if (!result.succeeded || expected.empty())
    {
        return false;
    }

    const std::string needle = ToLower(expected);
    if (ToLower(result.content).find(needle) != std::string::npos)
    {
        return true;
    }
    for (const std::string& entry : result.entries)
    {
        if (ToLower(entry).find(needle) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

std::uint64_t ElapsedMilliseconds(const std::chrono::steady_clock::time_point& startedAt)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt).count());
}

} // namespace

GoalRunner::GoalRunner(actions::ActionRuntime& runtime, const GoalStore& store)
    : actionRuntime(runtime), goalStore(store)
{
}

void GoalRunner::SetProgressHandler(ProgressHandler handler)
{
    progressHandler = std::move(handler);
}

void GoalRunner::SetConfirmationHandler(ConfirmationHandler handler)
{
    confirmationHandler = std::move(handler);
}

bool GoalRunner::Validate(const Goal& goal, std::string& outError)
{
    if (goal.steps.empty())
    {
        outError = "A goal must contain at least one step.";
        return false;
    }

    for (const GoalStep& step : goal.steps)
    {
        const std::string label = "Step " + std::to_string(step.ordinal);
        if (step.action.type == actions::ActionType::Unknown)
        {
            outError = label + " has no action.";
            return false;
        }
        if (step.check.type == actions::ActionType::Unknown)
        {
            outError = label + " has no verification action.";
            return false;
        }
        if (actions::RiskForAction(step.check.type) != actions::RiskLevel::ReadOnly)
        {
            outError = label + " verifies with " + actions::ToString(step.check.type) +
                ", which is not read-only. Verification must not change anything.";
            return false;
        }
        if (step.expected.empty())
        {
            outError = label + " does not say what success looks like.";
            return false;
        }
    }

    outError.clear();
    return true;
}

StopReason GoalRunner::CheckBudget(const Goal& goal)
{
    if (goal.budget.maxActions > 0 && goal.spend.actions >= goal.budget.maxActions)
    {
        return StopReason::BudgetActions;
    }
    if (goal.budget.maxDurationMs > 0 && goal.spend.elapsedMs >= goal.budget.maxDurationMs)
    {
        return StopReason::BudgetDuration;
    }
    if (goal.spend.retries > goal.budget.maxTotalRetries)
    {
        return StopReason::BudgetRetries;
    }
    if (goal.budget.maxTokens > 0 && goal.spend.tokens > goal.budget.maxTokens)
    {
        return StopReason::BudgetTokens;
    }
    return StopReason::None;
}

void GoalRunner::Publish(const Goal& goal, const GoalStep& step, const std::string& message) const
{
    if (!progressHandler)
    {
        return;
    }

    GoalProgress progress;
    progress.goalId = goal.id;
    progress.stepId = step.id;
    progress.ordinal = step.ordinal;
    progress.stepStatus = step.status;
    progress.goalStatus = goal.status;
    progress.message = message;
    progressHandler(progress);
}

bool GoalRunner::Persist(Goal& goal) const
{
    goal.updatedAt = std::chrono::system_clock::now();
    return goalStore.Save(goal);
}

bool GoalRunner::RunStep(
    Goal& goal,
    GoalStep& step,
    const policy::CapabilityPolicy& scopedPolicy,
    std::stop_token stopToken)
{
    const std::uint32_t maxAttempts = goal.budget.maxRetriesPerStep + 1;
    for (std::uint32_t attempt = 1; attempt <= maxAttempts; ++attempt)
    {
        if (stopToken.stop_requested())
        {
            goal.stopReason = StopReason::Cancelled;
            return false;
        }

        // Acting and observing cost two actions. Refusing to start a step that
        // cannot also be verified keeps a goal from ending in a state nobody
        // ever looked at.
        if (goal.budget.maxActions > 0 && goal.spend.actions + 2 > goal.budget.maxActions)
        {
            goal.stopReason = StopReason::BudgetActions;
            step.status = StepStatus::Failed;
            return false;
        }

        StepAttempt record;
        record.attempt = attempt;

        step.status = StepStatus::Acting;
        Publish(goal, step, "Attempt " + std::to_string(attempt) + ": " + step.description);

        actions::ActionRequest action = step.action;
        action.id = actions::NewActionId();
        action.requestedBy = "goal:" + goal.id;
        record.actionId = action.id;

        const actions::PolicyDecision decision =
            actionRuntime.EvaluateScoped(action, scopedPolicy);
        record.verdict = decision.verdict;

        if (decision.verdict == actions::PolicyVerdict::Blocked)
        {
            record.failure = decision.reason;
            step.attempts.push_back(std::move(record));
            step.status = StepStatus::Failed;
            goal.stopReason = StopReason::PolicyBlocked;
            Publish(goal, step, "Blocked by policy: " + decision.reason);
            return false;
        }

        bool confirmationGranted = false;
        if (decision.verdict == actions::PolicyVerdict::RequiresConfirmation)
        {
            confirmationGranted = confirmationHandler && confirmationHandler(action, decision);
            if (!confirmationGranted)
            {
                record.failure = "Confirmation was not granted.";
                step.attempts.push_back(std::move(record));
                step.status = StepStatus::Failed;
                goal.stopReason = StopReason::PolicyBlocked;
                Publish(goal, step, "Stopped: confirmation was not granted.");
                return false;
            }
        }

        const actions::ActionOutcome outcome =
            actionRuntime.ExecuteScoped(action, scopedPolicy, confirmationGranted);
        ++goal.spend.actions;
        record.executed = outcome.result.attempted;

        if (outcome.result.succeeded)
        {
            step.status = StepStatus::Verifying;
            Publish(goal, step, "Verifying: " + step.expected);

            actions::ActionRequest check = step.check;
            check.id = actions::NewActionId();
            check.requestedBy = "goal-check:" + goal.id;
            record.checkActionId = check.id;

            const actions::ActionOutcome checkOutcome =
                actionRuntime.ExecuteScoped(check, scopedPolicy, false);
            ++goal.spend.actions;
            record.observation = SummarizeResult(checkOutcome.result);
            record.verified = Observed(checkOutcome.result, step.expected);

            if (record.verified)
            {
                step.attempts.push_back(std::move(record));
                step.status = StepStatus::Succeeded;
                Publish(goal, step, "Verified: " + step.expected);
                return true;
            }
            record.failure = "Verification did not observe: " + step.expected;
        }
        else
        {
            record.failure = outcome.result.message.empty()
                ? std::string("The action did not succeed.")
                : outcome.result.message;
        }

        const std::string failure = record.failure;
        step.attempts.push_back(std::move(record));
        step.status = StepStatus::Failed;
        Publish(goal, step, failure);

        if (attempt < maxAttempts)
        {
            ++goal.spend.retries;
            if (goal.spend.retries > goal.budget.maxTotalRetries)
            {
                goal.stopReason = StopReason::BudgetRetries;
                return false;
            }
        }
    }

    goal.stopReason = StopReason::VerificationFailed;
    return false;
}

Goal GoalRunner::Run(Goal goal, std::stop_token stopToken)
{
    if (goal.id.empty())
    {
        goal.id = NewGoalId();
    }
    for (std::size_t index = 0; index < goal.steps.size(); ++index)
    {
        GoalStep& step = goal.steps[index];
        if (step.id.empty())
        {
            step.id = NewStepId();
        }
        step.ordinal = static_cast<std::uint32_t>(index);
    }

    std::string validationError;
    if (!Validate(goal, validationError))
    {
        goal.status = GoalStatus::Failed;
        goal.stopReason = StopReason::InvalidPlan;
        static_cast<void>(Persist(goal));
        return goal;
    }

    if (!actionRuntime.IsInitialized())
    {
        goal.status = GoalStatus::Blocked;
        goal.stopReason = StopReason::PolicyBlocked;
        static_cast<void>(Persist(goal));
        return goal;
    }

    const policy::CapabilityPolicy scopedPolicy(goal.scope);
    const auto startedAt = std::chrono::steady_clock::now();
    const std::uint64_t priorElapsed = goal.spend.elapsedMs;

    goal.status = GoalStatus::Running;
    goal.stopReason = StopReason::None;
    if (!Persist(goal))
    {
        goal.status = GoalStatus::Failed;
        goal.stopReason = StopReason::StoreError;
        return goal;
    }

    while (goal.currentStep < goal.steps.size())
    {
        goal.spend.elapsedMs = priorElapsed + ElapsedMilliseconds(startedAt);

        if (stopToken.stop_requested())
        {
            goal.status = GoalStatus::Cancelled;
            goal.stopReason = StopReason::Cancelled;
            static_cast<void>(Persist(goal));
            return goal;
        }

        const StopReason budget = CheckBudget(goal);
        if (budget != StopReason::None)
        {
            goal.status = GoalStatus::Exhausted;
            goal.stopReason = budget;
            static_cast<void>(Persist(goal));
            return goal;
        }

        GoalStep& step = goal.steps[goal.currentStep];
        if (step.status == StepStatus::Succeeded || step.status == StepStatus::Skipped)
        {
            ++goal.currentStep;
            continue;
        }

        if (!RunStep(goal, step, scopedPolicy, stopToken))
        {
            switch (goal.stopReason)
            {
                case StopReason::Cancelled:
                    goal.status = GoalStatus::Cancelled;
                    break;
                case StopReason::PolicyBlocked:
                    goal.status = GoalStatus::Blocked;
                    break;
                case StopReason::BudgetActions:
                case StopReason::BudgetDuration:
                case StopReason::BudgetRetries:
                case StopReason::BudgetTokens:
                    goal.status = GoalStatus::Exhausted;
                    break;
                default:
                    goal.status = GoalStatus::Failed;
                    break;
            }
            goal.spend.elapsedMs = priorElapsed + ElapsedMilliseconds(startedAt);
            static_cast<void>(Persist(goal));
            return goal;
        }

        ++goal.currentStep;
        goal.spend.elapsedMs = priorElapsed + ElapsedMilliseconds(startedAt);
        if (!Persist(goal))
        {
            goal.status = GoalStatus::Failed;
            goal.stopReason = StopReason::StoreError;
            return goal;
        }
    }

    goal.status = GoalStatus::Succeeded;
    goal.stopReason = StopReason::Completed;
    goal.spend.elapsedMs = priorElapsed + ElapsedMilliseconds(startedAt);
    static_cast<void>(Persist(goal));
    return goal;
}

Goal GoalRunner::Resume(const std::string& goalId, std::stop_token stopToken)
{
    const std::optional<Goal> stored = goalStore.Load(goalId);
    if (!stored.has_value())
    {
        Goal missing;
        missing.id = goalId;
        missing.status = GoalStatus::Failed;
        missing.stopReason = StopReason::StoreError;
        return missing;
    }

    Goal goal = stored.value();
    if (IsTerminal(goal.status))
    {
        return goal;
    }

    // A step caught mid-flight by a restart is retried, not assumed done: the
    // process died somewhere between acting and observing, so nothing about it
    // was ever proven.
    for (GoalStep& step : goal.steps)
    {
        if (step.status == StepStatus::Acting || step.status == StepStatus::Verifying)
        {
            step.status = StepStatus::Pending;
        }
    }
    return Run(std::move(goal), std::move(stopToken));
}

} // namespace revia::goals
