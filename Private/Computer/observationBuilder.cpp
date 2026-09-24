#include "Computer/observationBuilder.h"
#include "Core/utf8.h"

#include "Perception/windowEventMonitor.h"
#include "Policy/capabilityPolicy.h"

#include <algorithm>

namespace revia::computer
{

namespace
{

constexpr std::size_t MaximumObservationCharacters = 400;
constexpr std::size_t MaximumRecordedAttempts = 8;
// What fits in a decision prompt beside the goal and the history. A browser advertises
// thousands of elements; the whole tree is not a usable answer to "what is on screen",
// and an unbounded one would push the history out of context.
constexpr std::size_t MaximumListedControls = 40;

std::string Bounded(std::string text, const std::size_t limit)
{
    for (char& character : text)
    {
        if (character == '\r' || character == '\n') character = ' ';
    }
    if (text.size() > limit)
    {
        revia::utf8::Truncate(text, limit);
        text += "...";
    }
    return text;
}

} // namespace

std::vector<ObservedCandidate> BuildCandidates(
    const actions::windows::DesktopObservation& screen,
    const actions::CapabilitySettings& scope,
    const std::size_t maximumListedControls)
{
    std::vector<ObservedCandidate> candidates;
    const policy::CapabilityPolicy capability(scope);
    const std::size_t count = std::min(maximumListedControls, screen.controls.size());

    for (std::size_t index = 0; index < count; ++index)
    {
        const auto& control = screen.controls[index];
        if (!control.enabled) continue;

        // A password box never becomes a candidate, whatever else is true of it.
        //
        // This check used to be unnecessary by accident: the observer dropped every
        // unnamed element, and a named one called "Password" was caught downstream by
        // the consequence classifier reading its name. Admitting nameless controls
        // removes both of those, and a password box that publishes no name has nothing
        // for a name-based check to read. So it is refused here, on the one signal that
        // is not a guess.
        if (control.isPassword) continue;

        // What the executor will resolve. An automation id for a control that has one,
        // the name otherwise -- and a nameless control with neither cannot be acted on
        // at all, so it is not offered.
        const std::string target =
            control.automationId.empty() ? control.name : control.automationId;
        if (target.empty() || target.size() > 240) continue;

        // Constrain proposed operations to observed Windows patterns. This only removes
        // unusable choices; execution still rechecks policy and target identity.
        const auto permitted = [&](const actions::ActionType type)
        {
            actions::ActionRequest candidate;
            candidate.type = type;
            candidate.application = screen.foregroundApplication;
            candidate.control = target;
            candidate.value = " ";
            return capability.Evaluate(candidate).verdict != actions::PolicyVerdict::Blocked;
        };

        ObservedCandidate candidate;
        candidate.id = target;
        candidate.name = control.name;
        // The role in words rather than in Windows' numbering, so a descriptor can
        // narrow by kind without anything downstream knowing a control type id.
        candidate.role = ControlRoleName(control.controlType);
        candidate.inferredLabel = control.inferredLabel;
        candidate.container = control.containerName;
        candidate.nameless = control.nameless;
        candidate.mayInvoke =
            control.invokable && permitted(actions::ActionType::InvokeControl);
        candidate.maySetText =
            control.editable && permitted(actions::ActionType::SetControlText);
        candidate.mayType = control.editable && permitted(actions::ActionType::TypeText);
        if (!candidate.mayInvoke && !candidate.maySetText && !candidate.mayType)
        {
            continue;
        }
        candidates.push_back(std::move(candidate));
    }

    return candidates;
}

ComputerObservationBuilder::ComputerObservationBuilder(
    actions::windows::DesktopObserver& desktopObserver)
    : observer(&desktopObserver)
{
}

void ComputerObservationBuilder::Reset()
{
    lastObservation = actions::windows::DesktopObservation{};
    lastObservedScreen.clear();
}

ComputerTaskContext ComputerObservationBuilder::Build(
    const goals::Goal& goal,
    const std::uint32_t iteration,
    const perceptionSettings& perception)
{
    ComputerTaskContext context;
    context.subgoal = goal.title;
    context.iteration = iteration;
    context.stepsTaken = goal.steps.size();
    context.actionsLeft = goal.budget.maxActions > goal.spend.actions
        ? goal.budget.maxActions - goal.spend.actions : 0u;
    context.retriesLeft = goal.budget.maxTotalRetries > goal.spend.retries
        ? goal.budget.maxTotalRetries - goal.spend.retries : 0u;
    context.scope = goal.scope;

    // Newest attempts, because those are what the next decision turns on. An early
    // attempt that has already been superseded is the first thing worth dropping when
    // the history outgrows its room.
    std::size_t recorded = 0;
    for (auto step = goal.steps.rbegin();
         step != goal.steps.rend() && recorded < MaximumRecordedAttempts; ++step)
    {
        for (auto attempt = step->attempts.rbegin();
             attempt != step->attempts.rend() && recorded < MaximumRecordedAttempts;
             ++attempt, ++recorded)
        {
            ComputerAttempt record;
            record.description = step->description;
            record.action = step->action.type;
            record.expected = Bounded(step->expected, MaximumObservationCharacters);
            record.status = step->status;
            record.executed = attempt->executed;
            record.verified = attempt->verified;
            // What the check actually saw, which is the only part of this the next
            // decision may treat as fact.
            record.observed = Bounded(attempt->observation, MaximumObservationCharacters);
            record.failure = Bounded(attempt->failure, MaximumObservationCharacters);
            context.recentAttempts.push_back(std::move(record));
        }
    }
    std::reverse(context.recentAttempts.begin(), context.recentAttempts.end());

    // Look before deciding. Without this the loop chose its next action from the goal
    // and its own history alone -- it acted on the machine it expected rather than the
    // one in front of it, which is the failure the act/observe/verify cycle exists to
    // prevent.
    //
    // Exactly one look per iteration, handed to whoever decides. Observation is not
    // authority and does not become it here. Nothing below grants a permission, widens a
    // scope, or reaches an executor; every action this informs is still parsed, still
    // checked by CapabilityPolicy, still confirmed, still rate limited and still audited
    // exactly as before.
    context.observation.screen = observer->Observe();
    const actions::windows::DesktopObservation& screen = context.observation.screen;
    // The same exclusion list ordinary perception honours, for the same reason. A goal
    // run is not a reason to read the password manager the user just switched to, and
    // the window is reported as withheld rather than described, so the decision knows it
    // is blind here instead of concluding the screen is empty.
    context.observation.withheld = screen.succeeded &&
        perception::PerceptionFilter::IsExcludedWindow(
            perception, screen.foregroundApplication, screen.foregroundTitle);

    if (context.observation.Available())
    {
        context.observation.candidates =
            BuildCandidates(screen, goal.scope, MaximumListedControls);
    }

    // Kept only when it may actually be used. A withheld window leaves this empty, so
    // there is nothing for a visual target to be bound to and the exclusion cannot be
    // reached around.
    lastObservation = context.observation.Available()
        ? screen : actions::windows::DesktopObservation{};

    // Acting is not achieving. An action that ran, succeeded, and left the screen
    // identical has made no progress, and saying so is what stops the loop from
    // repeating it until a budget runs out.
    const std::string digest = screen.Fingerprint();
    if (iteration > 0 && !lastObservedScreen.empty())
    {
        context.observation.changedSinceLastDecision = digest != lastObservedScreen;
    }
    lastObservedScreen = digest;

    return context;
}

} // namespace revia::computer
