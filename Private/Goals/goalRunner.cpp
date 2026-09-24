#include "Goals/goalRunner.h"
#include "Core/utf8.h"

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
std::string SummarizeResult(const actions::ActionResult& result, const std::string& expected)
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
    // Keep the evidence that verified this step ahead of unrelated controls. The
    // next planner receives a bounded prefix, so list order must not erase success.
    const std::string needle = ToLower(expected);
    const auto matches = [&](const std::string& entry)
    {
        return !needle.empty() && ToLower(entry).find(needle) != std::string::npos;
    };
    for (const std::string& entry : result.entries)
        if (matches(entry)) append(entry);
    for (const std::string& entry : result.entries)
    {
        if (!matches(entry)) append(entry);
    }

    if (summary.size() > MaxObservationCharacters)
    {
        revia::utf8::Truncate(summary, MaxObservationCharacters);
        summary += "...";
    }
    return summary;
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

void GoalRunner::SetStepProvider(StepProvider provider)
{
    stepProvider = std::move(provider);
}

void GoalRunner::ClearStandingApproval()
{
    // Whatever the last run ended with is dropped; whatever this one was handed is taken
    // up, once. A seed that is never used by a run does not survive into a later one.
    blanketApproval = seededApproval;
    blanketRiskCeiling = seededCeiling;
    refuseAdditionalApproval = seededRefuseEscalation;
    seededApproval = false;
    seededRefuseEscalation = false;
    seededCeiling = actions::RiskLevel::ReadOnly;
}

void GoalRunner::SeedStandingApproval(const actions::RiskLevel ceiling, const bool refuseEscalation)
{
    seededApproval = true;
    seededCeiling = ceiling;
    seededRefuseEscalation = refuseEscalation;
}

void GoalRunner::SetConfirmationHandler(ConfirmationHandler handler)
{
    confirmationHandler = std::move(handler);
}

namespace
{

// Enough of an action to tell "she is doing the same thing again" from "she is doing
// something new". Deliberately covers the target as well as the verb: clicking twice in
// different places is progress, clicking twice in the same place is not.
std::string ActionFingerprint(const actions::ActionRequest& action)
{
    std::string fingerprint = actions::ToString(action.type);
    fingerprint += '|' + action.application;
    fingerprint += '|' + action.windowTitle;
    fingerprint += '|' + action.control;
    fingerprint += '|' + action.value;
    fingerprint += '|' + action.input.keys;
    fingerprint += '|' + actions::PathToUtf8(action.source);
    fingerprint += '|' + actions::PathToUtf8(action.destination);
    if (action.input.hasPoint)
    {
        fingerprint += '|' + std::to_string(action.input.x) + ',' +
            std::to_string(action.input.y);
    }
    return fingerprint;
}

} // namespace

Goal GoalRunner::Operate(Goal goal, std::stop_token stopToken)
{
    ClearStandingApproval();
    goal.stopDetail.clear();
    if (goal.id.empty())
    {
        goal.id = NewGoalId();
    }

    if (!stepProvider)
    {
        // Refusing beats running zero steps and reporting success, which is the shape
        // this failure would otherwise take.
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

    std::string lastFingerprint;
    std::uint32_t repeats = 0;
    for (std::uint32_t iteration = 0;; ++iteration)
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

        const NextStep next = stepProvider(goal, iteration);
        // Charged before the answer is used, so a decision costs its budget whether or
        // not it turns out to be usable. Planning that produces nothing is still
        // planning that was paid for, and a loop that only charged for successful
        // answers could ask forever (ISSUE-REVIA-0068).
        ++goal.spend.plannerRequests;
        if (next.costReported) goal.spend.tokens += next.tokens;
        else ++goal.spend.unreportedRequests;
        // Stop during inference wins over a late planner answer.
        if (stopToken.stop_requested())
        {
            goal.status = GoalStatus::Cancelled;
            goal.stopReason = StopReason::Cancelled;
            goal.spend.elapsedMs = priorElapsed + ElapsedMilliseconds(startedAt);
            static_cast<void>(Persist(goal));
            return goal;
        }
        if (!next.hasStep)
        {
            // Finished, stuck, and waiting on the person are three outcomes, not two.
            goal.status = next.finished ? GoalStatus::Succeeded : GoalStatus::Blocked;
            goal.stopReason = next.finished ? StopReason::Completed
                : next.needsInput ? StopReason::NeedsInput : StopReason::Undecided;
            goal.stopDetail = revia::utf8::Prefix(next.reason, MaxObservationCharacters);
            goal.spend.elapsedMs = priorElapsed + ElapsedMilliseconds(startedAt);
            static_cast<void>(Persist(goal));
            return goal;
        }

        GoalStep step = next.step;
        step.ordinal = static_cast<std::uint32_t>(goal.steps.size());
        std::string stepError;
        if (!ValidateStep(step, stepError))
        {
            // A step invented mid-run faces exactly the checks a planned one does, so
            // the loop cannot become a way to execute something unverifiable.
            goal.status = GoalStatus::Failed;
            goal.stopReason = StopReason::InvalidPlan;
            goal.stopDetail = revia::utf8::Prefix(stepError, MaxObservationCharacters);
            goal.spend.elapsedMs = priorElapsed + ElapsedMilliseconds(startedAt);
            static_cast<void>(Persist(goal));
            return goal;
        }

        const std::string fingerprint = ActionFingerprint(step.action);
        repeats = fingerprint == lastFingerprint ? repeats + 1 : 1;
        lastFingerprint = fingerprint;
        if (goal.budget.maxIdenticalSteps > 0 && repeats > goal.budget.maxIdenticalSteps)
        {
            // Checked before executing, so the action that would have been pointless is
            // not performed. A budget stops work that costs too much; this stops work
            // that achieves nothing, which a budget alone would let run to exhaustion.
            goal.status = GoalStatus::Blocked;
            goal.stopReason = StopReason::NoProgress;
            goal.spend.elapsedMs = priorElapsed + ElapsedMilliseconds(startedAt);
            static_cast<void>(Persist(goal));
            return goal;
        }

        if (step.id.empty())
        {
            step.id = NewStepId();
        }
        step.status = StepStatus::Pending;
        goal.currentStep = static_cast<std::uint32_t>(goal.steps.size());
        goal.steps.push_back(std::move(step));

        if (!RunStep(goal, goal.steps[goal.currentStep], scopedPolicy, stopToken))
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
}

bool GoalRunner::ValidateStep(const GoalStep& step, std::string& outError)
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
    // Evidence about a window has to come from that window.
    //
    // Read-only is not the same question as relevant. A web search is read-only and
    // says nothing whatever about whether text reached a browser's address bar, yet
    // its results are searched for `expected` exactly as a window inspection's are --
    // and a search for "facebook.com" finds "facebook.com" every time. That is a check
    // that cannot fail, which is a check that proves nothing.
    //
    // Narrow on purpose. Filesystem evidence for a desktop action stays allowed,
    // because saving a file and then listing the directory is a real proof and a
    // common one. What is refused is evidence from outside the machine entirely, and
    // evidence read out of a different application's window.
    if (actions::IsUiAutomationAction(step.action.type) ||
        actions::IsDesktopControlAction(step.action.type))
    {
        if (step.check.type == actions::ActionType::WebSearch)
        {
            outError = label + " verifies desktop work with a web search, which cannot "
                "observe the window it acted on.";
            return false;
        }
        if (actions::IsUiAutomationAction(step.check.type) &&
            !step.action.application.empty() && !step.check.application.empty() &&
            ToLower(step.action.application) != ToLower(step.check.application))
        {
            outError = label + " acts on " + step.action.application +
                " but verifies by looking at " + step.check.application + ".";
            return false;
        }
    }
    outError.clear();
    return true;
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
        if (!ValidateStep(step, outError))
        {
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
    // The ceiling that does not depend on the backend's cooperation. Reported as a
    // token budget because that is what it bounds; a run that ends here spent too many
    // decisions, whether or not anything counted them.
    if (goal.budget.maxPlannerRequests > 0 &&
        goal.spend.plannerRequests > goal.budget.maxPlannerRequests)
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
    // Derived here, from the step's own action and check, and never read from what
    // proposed the step. A postcondition supplied from outside would be the actor
    // grading itself, which is the one thing a check exists not to be.
    step.postcondition = DerivePostcondition(step);

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
        std::string actionResult;
        const auto cancelled = [&]()
        {
            if (!stopToken.stop_requested()) return false;
            record.failure = record.executed
                ? "Cancelled after the action was attempted. Action result: " + actionResult
                : "Cancelled before the action executed.";
            const std::string message = record.failure;
            step.status = record.verified ? StepStatus::Succeeded : StepStatus::Failed;
            step.attempts.push_back(std::move(record));
            goal.stopReason = StopReason::Cancelled;
            Publish(goal, step, message);
            return true;
        };
        const auto auditFailed = [&](const actions::ActionOutcome& outcome)
        {
            if (outcome.auditError.empty()) return false;
            record.failure = outcome.Message();
            const std::string message = record.failure;
            step.attempts.push_back(std::move(record));
            step.status = StepStatus::Failed;
            goal.stopReason = StopReason::StoreError;
            Publish(goal, step, message);
            return true;
        };

        // The baseline, for a step whose evidence is that the window responded.
        //
        // Taken with the step's own read-only check, immediately before acting, so the
        // before and the after are the same question asked twice. It costs one extra
        // inspection and it is the difference between a press being permanently
        // unverifiable and a press producing evidence about whether the right control
        // was chosen.
        //
        // Re-taken on every attempt rather than once per step: a retry acts on the
        // window as it is now, and comparing against a screen from two attempts ago
        // would report the previous attempt's effect as this one's.
        if (step.postcondition.kind == PostconditionKind::ControlStateChanged)
        {
            actions::ActionRequest baseline = step.check;
            baseline.id = actions::NewActionId();
            baseline.requestedBy = "goal-baseline:" + goal.id;
            const actions::ActionOutcome baselineOutcome =
                actionRuntime.ExecuteScoped(baseline, scopedPolicy, false, stopToken);
            if (!stopToken.stop_requested() || baselineOutcome.result.attempted)
            {
                ++goal.spend.actions;
            }
            if (auditFailed(baselineOutcome)) return false;
            // An unreadable baseline leaves the value empty, which evaluates to
            // Unknown. The step still runs; it simply cannot be verified this way, and
            // says so rather than passing.
            step.postcondition.value = baselineOutcome.result.succeeded
                ? CanonicalWindowState(baselineOutcome.result) : std::string();
            if (cancelled()) return false;
        }

        step.status = StepStatus::Acting;
        Publish(goal, step, "Attempt " + std::to_string(attempt) + ": " + step.description);
        if (cancelled()) return false;

        actions::ActionRequest action = step.action;
        action.id = actions::NewActionId();
        action.requestedBy = "goal:" + goal.id;
        record.actionId = action.id;

        actions::PolicyDecision decision =
            actionRuntime.EvaluateScoped(action, scopedPolicy);
        if (refuseAdditionalApproval && actions::AlwaysNeedsItsOwnConfirmation(action.type))
        {
            decision.verdict = actions::PolicyVerdict::Blocked;
            decision.reason = "Deletion is outside this task's approval.";
        }
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
            // A standing yes covers only what it was given for: this run, and work no
            // riskier than the action the person actually saw. A step that escalates
            // asks again, which is the difference between answering once and signing a
            // blank cheque.
            if (blanketApproval &&
                !actions::AlwaysNeedsItsOwnConfirmation(action.type) &&
                static_cast<int>(decision.risk) <= static_cast<int>(blanketRiskCeiling))
            {
                confirmationGranted = true;
            }
            else if (confirmationHandler && !refuseAdditionalApproval)
            {
                const actions::ConfirmationChoice choice =
                    confirmationHandler(action, decision);
                confirmationGranted = actions::Granted(choice);
                if (choice == actions::ConfirmationChoice::AllowForThisTask)
                {
                    blanketApproval = true;
                    blanketRiskCeiling = decision.risk;
                }
            }
            if (cancelled()) return false;
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
            actionRuntime.ExecuteScoped(action, scopedPolicy, confirmationGranted, stopToken);
        if (!stopToken.stop_requested() || outcome.result.attempted) ++goal.spend.actions;
        record.executed = outcome.result.attempted;
        actionResult = outcome.Message();
        if (auditFailed(outcome)) return false;
        if (cancelled()) return false;

        if (outcome.result.succeeded)
        {
            step.status = StepStatus::Verifying;
            Publish(goal, step, "Verifying: " + step.expected);

            actions::ActionRequest check = step.check;
            check.id = actions::NewActionId();
            check.requestedBy = "goal-check:" + goal.id;
            record.checkActionId = check.id;

            const actions::ActionOutcome checkOutcome =
                actionRuntime.ExecuteScoped(check, scopedPolicy, false, stopToken);
            if (!stopToken.stop_requested() || checkOutcome.result.attempted) ++goal.spend.actions;
            record.observation = SummarizeResult(checkOutcome.result, step.expected);
            if (auditFailed(checkOutcome)) return false;
            // Judged under the goal's own contract, not the build's. A goal resumed
            // from a database written by an older build keeps the rule it was written
            // with; see Goal::verificationSchema.
            const VerificationJudgement judgement = JudgeStep(
                goal.verificationSchema, step.postcondition, step.expected,
                checkOutcome.result);
            record.checkedBy = judgement.checkedBy;
            record.outcome = judgement.outcome;
            record.expectedTextSeen = judgement.expectedTextSeen;
            record.verified = record.outcome == VerificationOutcome::Verified;
            if (cancelled()) return false;

            if (record.verified)
            {
                step.attempts.push_back(std::move(record));
                step.status = StepStatus::Succeeded;
                Publish(goal, step, "Verified: " + step.expected);
                if (stopToken.stop_requested())
                {
                    goal.stopReason = StopReason::Cancelled;
                    return false;
                }
                return true;
            }
            record.failure = record.outcome == VerificationOutcome::Failed
                ? "Verification observed that it did not happen: " + step.expected
                : "Verification could not establish: " + step.expected;
        }
        else
        {
            record.failure = outcome.result.message.empty()
                ? std::string("The action did not succeed.")
                : outcome.result.message;
        }

        // The action ran, said it worked, and the check could not confirm it. That is
        // an unknown outcome, not a failed one, and for an action that commits
        // something the two readings are opposite: if nothing landed, retrying is the
        // recovery; if something landed and only the evidence is missing, retrying does
        // it twice. So the run stops with a reason that says so rather than sending the
        // message again on the chance that the first one did not arrive
        // (ISSUE-REVIA-0062).
        //
        // Two things are not this case. An action that failed outright committed
        // nothing and retries exactly as before. And a typed postcondition that
        // positively answered "no" is a real answer -- the effect demonstrably did not
        // land, so doing it again does it once rather than twice. TextObserved can
        // never reach that branch, because not finding a substring is not a finding,
        // and that is precisely what a typed postcondition buys here.
        const bool effectMayHaveLanded = outcome.result.succeeded &&
            actions::RepeatingCouldDuplicateAnEffect(action.type) &&
            record.outcome != VerificationOutcome::Failed;

        const std::string failure = record.failure;
        step.attempts.push_back(std::move(record));
        step.status = StepStatus::Failed;
        Publish(goal, step, failure);

        if (effectMayHaveLanded)
        {
            goal.stopReason = StopReason::UnverifiedEffect;
            Publish(goal, step,
                "Stopped without repeating it: " + actions::ToString(action.type) +
                " reported success but could not be verified, and doing it again could "
                "do it twice.");
            return false;
        }

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
    ClearStandingApproval();
    goal.stopDetail.clear();
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
    // Cleared here too, deliberately. Resuming is a new sitting and a standing yes given
    // in an earlier run -- possibly in an earlier process -- is not consent given now.
    ClearStandingApproval();
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
    //
    // Not proven is not the same as not done, though, and that distinction decides
    // whether retrying is recovery or repetition. The process died somewhere between
    // sending the keystrokes and reading the result; from here those two are
    // indistinguishable. So a step whose action would only re-reach a state is retried
    // as before, and a step whose action commits something stops for a person to look
    // at instead of being replayed into a machine nobody has watched since.
    for (GoalStep& step : goal.steps)
    {
        if (step.status != StepStatus::Acting && step.status != StepStatus::Verifying)
        {
            continue;
        }
        if (actions::RepeatingCouldDuplicateAnEffect(step.action.type))
        {
            // Terminal rather than blocked, and the step keeps the status it was caught
            // in. Blocked is resumable, and a second resume would find the step no
            // longer mid-flight and replay it -- which is the thing this refuses to do.
            // Ending the goal means a person decides what actually happened and starts
            // the work again themselves, which is what reconciliation is.
            goal.status = GoalStatus::Failed;
            goal.stopReason = StopReason::UnverifiedEffect;
            goal.stopDetail = "Stopped after a restart: " +
                actions::ToString(step.action.type) + " was in flight when the process "
                "ended, and repeating it could do it twice.";
            static_cast<void>(Persist(goal));
            return goal;
        }
        step.status = StepStatus::Pending;
    }
    return Run(std::move(goal), std::move(stopToken));
}

} // namespace revia::goals
