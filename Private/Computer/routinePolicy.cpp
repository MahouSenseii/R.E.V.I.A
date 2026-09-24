#include "Computer/routinePolicy.h"

#include "Computer/subgoalValidator.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace revia::computer
{

namespace
{

std::string Lowered(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool Contains(const std::string& haystack, const std::string& needle)
{
    return Lowered(haystack).find(Lowered(needle)) != std::string::npos;
}

bool SameApplication(const std::string& left, const std::string& right)
{
    return !left.empty() && Lowered(left) == Lowered(right);
}

ComputerDecision Answer(
    const ComputerDecisionKind kind,
    const ComputerReasonCode code,
    std::string detail)
{
    ComputerDecision decision;
    decision.kind = kind;
    decision.code = code;
    decision.detail = std::move(detail);
    decision.provider = "routine";
    // Nothing was asked of a backend, so nothing was spent. Reported rather than left
    // at the default, because "this decision cost no tokens" is the measurement the
    // whole feature exists to produce, and an unreported zero is indistinguishable from
    // a backend that declined to say.
    decision.tokens = 0;
    decision.costReported = true;
    return decision;
}

// Whether this subgoal's own work has already been done and checked.
//
// Focus and launch can ask the screen -- the window either is in front or is not. An
// entry or an interaction cannot: the policy never sees a control's value (it does not
// hold the payload, and the observation carries no values), so "is the text already
// there?" is not a question it can put to the screen.
//
// What it can read is the run's own record. The runner verifies every step against a
// typed postcondition before recording it, so a verified attempt is the runtime's
// finding rather than the policy's opinion -- and matching on the description the policy
// itself wrote ties that finding to this subgoal's target rather than to whatever the
// previous subgoal touched.
//
// Without this the policy proposes the same correct action forever: the step works, the
// screen looks the same afterwards, and the run stops on no-progress having succeeded.
bool AlreadyDoneAndVerified(
    const ComputerTaskContext& context,
    const actions::ActionType expected,
    const std::string& targetName)
{
    if (context.recentAttempts.empty()) return false;
    const ComputerAttempt& latest = context.recentAttempts.back();
    if (!latest.executed || !latest.verified) return false;
    if (latest.action != expected) return false;
    if (targetName.empty()) return false;
    return Contains(latest.description, targetName);
}

} // namespace

RoutineComputerPolicy::RoutineComputerPolicy(const PayloadVault& payloadVault)
    : vault(&payloadVault)
{
}

void RoutineComputerPolicy::SetSubgoal(ComputerSubgoal subgoal)
{
    activeSubgoal = std::move(subgoal);
}

void RoutineComputerPolicy::ClearSubgoal()
{
    activeSubgoal = ComputerSubgoal{};
}

bool RoutineComputerPolicy::CoversIntent(const SubgoalIntent intent) const
{
    switch (intent)
    {
        case SubgoalIntent::LaunchApplication:
        case SubgoalIntent::FocusWindow:
        case SubgoalIntent::ResolveTarget:
        case SubgoalIntent::InteractWithControl:
        case SubgoalIntent::EnterPayload:
        case SubgoalIntent::WaitForState:
        case SubgoalIntent::Escalate:
            return true;
        case SubgoalIntent::Unspecified:
        default:
            return false;
    }
}

TargetMatch RoutineComputerPolicy::FindTarget(
    const ComputerTaskContext& context, const TargetAffordance affordance) const
{
    return MatchTarget(context.observation.candidates, activeSubgoal.target, affordance);
}

ComputerDecision RoutineComputerPolicy::Decide(
    const ComputerTaskContext& context, std::stop_token stopToken)
{
    // A stop that arrived before the work started still stops it. Cheap here, and the
    // habit matters more in the policies that are not.
    if (stopToken.stop_requested())
    {
        return Answer(ComputerDecisionKind::CannotHandle, ComputerReasonCode::None,
            "The task was stopped.");
    }

    if (!activeSubgoal.Validated())
    {
        // No typed goal, so nothing to be deterministic about. Inferring one from the
        // goal's free-text title is exactly the open-ended reading that stays with Main.
        return Answer(ComputerDecisionKind::CannotHandle,
            ComputerReasonCode::OutsideQualifiedScope,
            "This step has no bounded subgoal for a routine decision to work from.");
    }
    if (!CoversIntent(activeSubgoal.intent))
    {
        return Answer(ComputerDecisionKind::CannotHandle,
            ComputerReasonCode::OutsideQualifiedScope,
            "This subgoal is outside what a routine decision covers.");
    }

    const actions::windows::DesktopObservation& screen = context.observation.screen;
    const bool canSee = context.observation.Available();
    const std::string& application = activeSubgoal.target.application;

    switch (activeSubgoal.intent)
    {
        case SubgoalIntent::Escalate:
            return Answer(ComputerDecisionKind::NeedReasoning, ComputerReasonCode::None,
                activeSubgoal.description.empty()
                    ? "This subgoal asks for reasoning rather than a routine step."
                    : activeSubgoal.description);

        case SubgoalIntent::WaitForState:
            if (!canSee)
            {
                return Answer(ComputerDecisionKind::Reobserve,
                    ComputerReasonCode::ObservationUnavailable,
                    "Waiting, and the screen could not be read this time.");
            }
            return Answer(ComputerDecisionKind::WaitForState, ComputerReasonCode::None,
                "Something is still in progress on screen.");

        case SubgoalIntent::LaunchApplication:
        {
            // Permitted without an observation, and only this one is. A launch resolves
            // from the approved application list rather than from what is on screen, so
            // a blind moment is not a reason to refuse it -- while every intent below
            // needs to see what it is aiming at.
            if (canSee && SameApplication(screen.foregroundApplication, application))
            {
                return Answer(ComputerDecisionKind::ProposeCompletion,
                    ComputerReasonCode::None,
                    application + " is already in front.");
            }
            if (!ScopeApprovesApplication(activeSubgoal.scope, application) ||
                !activeSubgoal.scope.desktopControl.applicationLaunch)
            {
                // Refused here rather than proposed and refused downstream. The
                // executor would stop it either way; proposing it anyway would spend an
                // action budget and an audit line to be told what was already known.
                return Answer(ComputerDecisionKind::CannotHandle,
                    ComputerReasonCode::OutsideQualifiedScope,
                    "Starting " + application + " is not approved for this task.");
            }

            ComputerDecision decision = Answer(ComputerDecisionKind::ProposeAction,
                ComputerReasonCode::None, "Start " + application + ".");
            decision.step.description = "Start " + application;
            decision.step.action.type = actions::ActionType::LaunchApplication;
            decision.step.action.application = application;
            decision.step.check.type = actions::ActionType::InspectWindow;
            decision.step.check.application = application;
            decision.step.expected = application + " is in front";
            return decision;
        }

        case SubgoalIntent::FocusWindow:
        {
            if (!canSee)
            {
                return Answer(ComputerDecisionKind::Reobserve,
                    ComputerReasonCode::ObservationUnavailable,
                    "The screen could not be read, so what is in front is unknown.");
            }
            if (SameApplication(screen.foregroundApplication, application))
            {
                return Answer(ComputerDecisionKind::ProposeCompletion,
                    ComputerReasonCode::None, application + " is already in front.");
            }

            ComputerDecision decision = Answer(ComputerDecisionKind::ProposeAction,
                ComputerReasonCode::None, "Bring " + application + " to the front.");
            decision.step.description = "Focus " + application;
            decision.step.action.type = actions::ActionType::FocusWindow;
            decision.step.action.application = application;
            decision.step.action.windowTitle = activeSubgoal.target.windowTitle;
            decision.step.check.type = actions::ActionType::InspectWindow;
            decision.step.check.application = application;
            decision.step.expected = application + " is in front";
            return decision;
        }

        case SubgoalIntent::ResolveTarget:
        case SubgoalIntent::InteractWithControl:
        case SubgoalIntent::EnterPayload:
        {
            if (!canSee)
            {
                // Withheld and failed are different blindnesses. A window the
                // perception filter excluded will still be excluded on the next look,
                // so asking to look again would loop; existing vision is the route that
                // has its own rules about that.
                return context.observation.withheld
                    ? Answer(ComputerDecisionKind::NeedVision,
                        ComputerReasonCode::ObservationUnavailable,
                        "The window in front is excluded from observation.")
                    : Answer(ComputerDecisionKind::Reobserve,
                        ComputerReasonCode::ObservationUnavailable,
                        "The screen could not be read this time.");
            }

            // The target lives in a particular window, and acting into the wrong one is
            // the failure this check exists to prevent. Focusing first is bounded local
            // progress toward the same subgoal rather than a different task.
            if (!SameApplication(screen.foregroundApplication, application))
            {
                ComputerDecision decision = Answer(ComputerDecisionKind::ProposeAction,
                    ComputerReasonCode::None,
                    "Bring " + application + " to the front before touching it.");
                decision.step.description = "Focus " + application;
                decision.step.action.type = actions::ActionType::FocusWindow;
                decision.step.action.application = application;
                decision.step.action.windowTitle = activeSubgoal.target.windowTitle;
                decision.step.check.type = actions::ActionType::InspectWindow;
                decision.step.check.application = application;
                decision.step.expected = application + " is in front";
                return decision;
            }

            // The window title, when one was asked for, is part of which window this is
            // and not a hint. A conversation with the wrong person is the right control
            // in the wrong window.
            if (!activeSubgoal.target.windowTitle.empty() &&
                !Contains(screen.foregroundTitle, activeSubgoal.target.windowTitle))
            {
                return Answer(ComputerDecisionKind::NeedReasoning,
                    ComputerReasonCode::OutsideQualifiedScope,
                    "The window in front is not the one this subgoal names.");
            }

            // The affordance follows the operation. Resolving reports a control and
            // touches nothing, so it looks at every candidate; the other two need the
            // control to actually support what they are about to do to it.
            const TargetAffordance affordance =
                activeSubgoal.intent == SubgoalIntent::EnterPayload
                    ? TargetAffordance::Editable
                : activeSubgoal.intent == SubgoalIntent::ResolveTarget
                    ? TargetAffordance::Any
                    : TargetAffordance::Invokable;
            const TargetMatch match = FindTarget(context, affordance);

            if (match.outcome == TargetMatch::Outcome::Ambiguous)
            {
                // A person decides which one. Guessing here is how a message goes to
                // the wrong recipient, and no amount of confidence substitutes for
                // being told.
                return Answer(ComputerDecisionKind::NeedUser, ComputerReasonCode::None,
                    "More than one thing on screen matches that description.");
            }
            if (match.outcome == TargetMatch::Outcome::NotFound)
            {
                // A bounded listing that left things out is not a statement about what
                // is missing from it. Looking again is the cheap answer; concluding the
                // target does not exist is the expensive mistake.
                if (screen.omittedControls > 0)
                {
                    return Answer(ComputerDecisionKind::Reobserve,
                        ComputerReasonCode::ObservationUnavailable,
                        "The target was not in what was listed, and the listing was "
                        "capped.");
                }
                return Answer(ComputerDecisionKind::NeedReasoning,
                    ComputerReasonCode::OutsideQualifiedScope,
                    "Nothing on screen matches that description.");
            }

            if (activeSubgoal.intent == SubgoalIntent::ResolveTarget)
            {
                // Read-only by construction: the answer is that it is there, and
                // reporting it is the whole subgoal.
                return Answer(ComputerDecisionKind::ProposeCompletion,
                    ComputerReasonCode::None,
                    "Found " + match.candidate->Describe() + " in " + application + ".");
            }

            if (activeSubgoal.intent == SubgoalIntent::InteractWithControl)
            {
                if (AlreadyDoneAndVerified(context, actions::ActionType::InvokeControl,
                        match.candidate->Describe()))
                {
                    return Answer(ComputerDecisionKind::ProposeCompletion,
                        ComputerReasonCode::None,
                        match.candidate->Describe() + " has already been used.");
                }
                ComputerDecision decision = Answer(ComputerDecisionKind::ProposeAction,
                    ComputerReasonCode::None,
                    "Use " + match.candidate->Describe() + ".");
                decision.step.description =
                    "Use " + match.candidate->Describe() + " in " + application;
                decision.step.action.type = actions::ActionType::InvokeControl;
                decision.step.action.application = application;
                decision.step.action.control = match.candidate->id;
                decision.step.check.type = actions::ActionType::InspectWindow;
                decision.step.check.application = application;
                decision.step.expected = match.candidate->Describe() + " (" +
                    match.candidate->id + ") was used in " + application;
                return decision;
            }

            // EnterPayload.
            if (AlreadyDoneAndVerified(context, actions::ActionType::SetControlText,
                    match.candidate->Describe()))
            {
                return Answer(ComputerDecisionKind::ProposeCompletion,
                    ComputerReasonCode::None,
                    match.candidate->Describe() + " already holds the prepared content.");
            }

            // The reference is checked for substance and the value is never read: the
            // controller redeems it after this decision has been accepted, which is
            // what keeps the user's exact words out of a policy.
            if (!activeSubgoal.payload.Valid() ||
                vault == nullptr || !vault->Holds(activeSubgoal.payload))
            {
                return Answer(ComputerDecisionKind::CannotHandle,
                    ComputerReasonCode::OutsideQualifiedScope,
                    "The content this subgoal would enter is not available.");
            }

            ComputerDecision decision = Answer(ComputerDecisionKind::ProposeAction,
                ComputerReasonCode::None,
                "Put the prepared content into " + match.candidate->Describe() + ".");
            decision.step.description =
                "Fill " + match.candidate->Describe() + " in " + application;
            decision.step.action.type = actions::ActionType::SetControlText;
            decision.step.action.application = application;
            decision.step.action.control = match.candidate->id;
            // Deliberately left empty. The controller fills it from the vault; a policy
            // that wrote the value here would be a policy that had it.
            decision.step.action.value.clear();
            decision.step.check.type = actions::ActionType::InspectWindow;
            decision.step.check.application = application;
            // Names the box rather than quoting the content, so the user's own words do
            // not reach the activity feed or the stored goal record on their way to
            // being verified. The typed postcondition still checks the real value,
            // because it is derived from the action after the value is substituted.
            //
            // The identifier is here as well as the readable name, and it has to be.
            // Verification asks whether the step's description is about the same thing
            // its action touched, and it can only compare what it is given: the
            // postcondition's subject is the control as the *action* names it. A
            // description carrying only the friendly name has nothing in common with
            // it, so a step that worked perfectly would be judged uncorroborated and
            // the run would stop on an effect it had in fact verified.
            decision.step.expected = match.candidate->Describe() + " (" +
                match.candidate->id + ") holds the prepared content";
            decision.payload = activeSubgoal.payload;
            return decision;
        }

        case SubgoalIntent::Unspecified:
        default:
            return Answer(ComputerDecisionKind::CannotHandle,
                ComputerReasonCode::OutsideQualifiedScope,
                "This subgoal is outside what a routine decision covers.");
    }
}

} // namespace revia::computer
