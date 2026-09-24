#include "Computer/contentGate.h"
#include "Core/utf8.h"
#include "Policy/desktopAuthorization.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace revia::computer
{

namespace
{

// How much of a refused value is worth keeping. Enough to recognise an invented
// placeholder in a record, short enough that a log never becomes a second copy of
// whatever a planner decided to write.
constexpr std::size_t MaximumAttemptedExcerpt = 80;

[[nodiscard]] std::string Lowered(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

[[nodiscard]] bool Contains(const std::string& haystack, const std::string& needle)
{
    if (needle.empty()) return false;
    return Lowered(haystack).find(Lowered(needle)) != std::string::npos;
}

[[nodiscard]] std::string Excerpt(const std::string& value)
{
    std::string bounded = value;
    for (char& character : bounded)
    {
        if (character == '\r' || character == '\n') character = ' ';
    }
    if (bounded.size() > MaximumAttemptedExcerpt)
    {
        revia::utf8::Truncate(bounded, MaximumAttemptedExcerpt);
        bounded += "...";
    }
    return bounded;
}

[[nodiscard]] bool EntersText(const actions::ActionType type)
{
    return type == actions::ActionType::SetControlText ||
        type == actions::ActionType::TypeText;
}

// Whether the written text looks like an attempt at the held value rather than
// something composed from nothing. Containment either way: a truncation, an extension,
// a quoted copy. Used only to label the record -- both outcomes place the held value.
[[nodiscard]] bool ResemblesHeld(const std::string& written, const std::string& held)
{
    if (written.empty() || held.empty()) return false;
    return Contains(written, held) || Contains(held, written);
}

} // namespace

std::string ToString(const ContentOutcome value)
{
    switch (value)
    {
        case ContentOutcome::Supplied: return "supplied";
        case ContentOutcome::DraftAdopted: return "draft_adopted";
        case ContentOutcome::InventedText: return "invented_text";
        case ContentOutcome::ModifiedPayload: return "modified_payload";
        case ContentOutcome::MissingPayload: return "missing_payload";
        case ContentOutcome::WrongDestination: return "wrong_destination";
        case ContentOutcome::StaleDraft: return "stale_draft";
        case ContentOutcome::NotApplicable:
        default: return "not_applicable";
    }
}

ContentGate::ContentGate(PayloadVault& payloadVault, DraftObserver observer)
    : vault(&payloadVault), observeDraft(std::move(observer))
{
}

void ContentGate::BeginTask(TaskContent content)
{
    task = std::move(content);
    held = PayloadReference{};
    placed = false;
    placement = {};
    draftBaseline = {};
    baselineBeforeEntry = false;
    submissionExecuted = false;
    submissionVerified = false;
    stats = ContentGateStats{};

    // Custody is taken here, before anything is planned. The ordering is the guarantee:
    // by the time any model is asked what to do, the words it might otherwise have
    // invented are already held by something it cannot reach.
    if (task.RequiresExactContent() && !task.value.empty() && vault != nullptr)
    {
        held = vault->Store(task.value, task.kind.empty() ? "text" : task.kind,
            ContentProvenance::UserSupplied);
    }
}

void ContentGate::EndTask()
{
    task = TaskContent{};
    held = PayloadReference{};
    placed = false;
    placement = {};
    draftBaseline = {};
    baselineBeforeEntry = false;
    submissionExecuted = false;
    submissionVerified = false;
}

void ContentGate::ObserveGoal(const goals::Goal& goal)
{
    placed = false;
    // Sending may clear the draft. Only a runtime-guarded, verified commit is terminal
    // evidence; an unverified execution must not be repeated or reported as verified.
    for (const auto& step : goal.steps)
    {
        if (!task.submissionRequested || !step.action.submissionStarted ||
            !step.action.submissionStarted->load()) continue;
        submissionExecuted = true;
        for (const auto& attempt : step.attempts)
        {
            if (!attempt.executed) continue;
            submissionVerified = submissionVerified ||
                attempt.outcome == goals::VerificationOutcome::Verified;
        }
    }
    if (!held.Valid() || vault == nullptr || !observeDraft) return;
    const std::optional<std::string> value = vault->Redeem(held);
    if (!value.has_value()) return;

    for (auto entry = goal.steps.rbegin(); entry != goal.steps.rend(); ++entry)
    {
        const goals::GoalStep& step = *entry;
        if (!EntersText(step.action.type)) continue;
        if (step.action.value != *value) continue;
        // Verified, not merely executed. An action that ran and could not be checked
        // leaves the question open, and treating an open question as a placement is how
        // a task reports success it never observed.
        const bool verified = std::any_of(step.attempts.begin(), step.attempts.end(),
            [](const goals::StepAttempt& attempt) {
                return attempt.outcome == goals::VerificationOutcome::Verified;
            });
        if (verified)
        {
            const auto current = observeDraft(step.action);
            if (!draftBaseline.valid && !task.submissionRequested)
                draftBaseline = current;
            placed = actions::windows::CompareDraftSnapshots(
                draftBaseline, current, *value).empty();
            if (placed) placement = step.action;
            return;
        }
    }
}

bool ContentGate::CompletionAllowed() const
{
    // A task with nothing identifiable to place is finished when the provider and the
    // runner's evidence check say it is, exactly as before.
    if (!task.Any()) return true;
    return task.submissionRequested ? submissionVerified : placed;
}

void ContentGate::NoteRefusedCompletion()
{
    ++stats.prematureCompletions;
}

ContentGate::DestinationVerdict ContentGate::CheckDestination(
    const std::string& control, const ComputerTaskContext& context) const
{
    if (task.destination.empty()) return DestinationVerdict::Unchecked;
    if (control.empty()) return DestinationVerdict::Unchecked;

    const auto found = std::find_if(context.observation.candidates.begin(),
        context.observation.candidates.end(),
        [&](const ObservedCandidate& candidate) { return candidate.id == control; });
    // A control the observation does not describe is one this cannot judge. The
    // executor still refuses a target it cannot re-find; inventing a mismatch here
    // would stop a task on the strength of a listing that was merely capped.
    if (found == context.observation.candidates.end())
    {
        return DestinationVerdict::Unchecked;
    }

    // Matched against every way the observation can name the control, because a user
    // names a field the way they read it: by its label, by its inferred label, or by
    // the panel it sits in.
    const bool matches = Contains(found->name, task.destination) ||
        Contains(found->inferredLabel, task.destination) ||
        Contains(found->container, task.destination) ||
        Contains(task.destination, found->name);
    return matches ? DestinationVerdict::Matches : DestinationVerdict::Mismatch;
}

ContentDecision ContentGate::Apply(
    goals::GoalStep& step, const ComputerTaskContext& context)
{
    ContentDecision decision;
    if (!EntersText(step.action.type))
    {
        if (!task.Any()) return decision;
        bool activates = step.action.type == actions::ActionType::InvokeControl ||
            step.action.type == actions::ActionType::ClickPointer ||
            step.action.type == actions::ActionType::DragPointer;
        if (step.action.type == actions::ActionType::PressKeys)
        {
            const std::string keys = Lowered(step.action.input.keys);
            const std::vector<std::string> navigation = {"tab", "shift+tab", "escape",
                "left", "right", "up", "down", "home", "end", "pageup", "pagedown",
                "ctrl+a", "ctrl+c", "ctrl+f", "f3"};
            activates = std::find(navigation.begin(), navigation.end(), keys) == navigation.end();
        }
        // A named editable field may be focused before entry. Unknown pointer targets
        // cannot claim this exception, because they could be a submission control.
        if (step.action.type == actions::ActionType::ClickPointer)
        {
            const auto field = std::find_if(context.observation.candidates.begin(),
                context.observation.candidates.end(), [&](const auto& candidate) {
                    return candidate.id == step.action.control &&
                        (candidate.mayType || candidate.maySetText);
                });
            if (context.observation.Available() && field != context.observation.candidates.end())
            {
                activates = false;
                step.action.requiresRuntimeGuard = true;
                step.action.navigationConstraint = {true, true, field->name,
                    context.observation.screen.foregroundTitle};
            }
        }
        if (activates && step.action.type != actions::ActionType::PressKeys &&
            step.action.type != actions::ActionType::DragPointer && context.observation.Available())
        {
            const auto candidate = std::find_if(context.observation.candidates.begin(),
                context.observation.candidates.end(), [&](const auto& item) {
                    return item.id == step.action.control;
                });
            if (candidate != context.observation.candidates.end())
            {
                policy::AuthorizationRequest effect;
                effect.operation = policy::DesktopOperation::Invoke;
                effect.evidence.resolved = true;
                effect.evidence.controlName = candidate->name;
                effect.evidence.windowTitle = context.observation.screen.foregroundTitle;
                // Use the shared consequence classifier so named recipients and
                // conversation rows remain selectable before a draft exists.
                if (policy::AssessEffects(effect) == 0u &&
                    policy::AssessEvidence(effect) == policy::EvidenceQuality::Verified)
                {
                    activates = false;
                    step.action.requiresRuntimeGuard = true;
                    step.action.navigationConstraint = {true, false, candidate->name,
                        context.observation.screen.foregroundTitle};
                }
            }
        }
        if (!activates) return decision;
        const auto value = vault && held.Valid() ? vault->Redeem(held) : std::nullopt;
        if (submissionExecuted || !task.submissionRequested || !SubmissionReady() || !value || !observeDraft)
        {
            decision.allowed = false;
            decision.outcome = ContentOutcome::StaleDraft;
            decision.detail = "Submission needs authorization and a current, exact draft in its original context.";
            return decision;
        }
        const auto baseline = draftBaseline;
        const auto read = observeDraft;
        auto target = placement;
        target.beforeCommit = {};
        target.onCommitStarted = {};
        target.submissionStarted.reset();
        step.action.beforeCommit = [baseline, read, target, expected = *value]() {
            return actions::windows::CompareDraftSnapshots(baseline, read(target), expected);
        };
        step.action.requiresRuntimeGuard = true;
        step.action.navigationConstraint = {};
        const std::string keys = Lowered(step.action.input.keys);
        const bool keyboard = step.action.type == actions::ActionType::PressKeys;
        const bool submitKey = keyboard &&
            (keys == "enter" || keys == "ctrl+enter" || keys == "alt+s");
        step.action.submissionStarted = std::make_shared<std::atomic_bool>(false);
        const auto started = step.action.submissionStarted;
        step.action.onCommitStarted = [started, submitKey, keyboard](const std::string& observedName) {
            policy::AuthorizationRequest effect;
            effect.operation = policy::DesktopOperation::Invoke;
            effect.evidence.resolved = true;
            effect.evidence.controlName = observedName;
            if (started && (submitKey || (!keyboard && policy::HasEffect(policy::AssessEffects(effect),
                    policy::DesktopEffect::ExternalMessage))))
                started->store(true);
        };
        const std::string drift = step.action.beforeCommit();
        if (!drift.empty())
        {
            placed = false;
            decision.allowed = false;
            decision.outcome = ContentOutcome::StaleDraft;
            decision.detail = "Submission refused: " + drift + ".";
        }
        return decision;
    }

    const bool wrotePlaceholder = step.action.value == PreparedContentToken;

    if (!task.Any())
    {
        // No identifiable content, so this is judged exactly as it was before the gate
        // existed -- except for one thing. The placeholder is the grammar's way of
        // saying "the runtime supplies this", and typing it literally into a field is
        // the single worst outcome available here.
        if (wrotePlaceholder)
        {
            ++stats.missingPayloads;
            decision.outcome = ContentOutcome::MissingPayload;
            decision.allowed = false;
            decision.needsInput = true;
            decision.detail =
                "The step asked to enter prepared content and this task has none.";
        }
        return decision;
    }

    // Destination before content. Placing the user's exact words in a field they did
    // not name is not a smaller mistake than placing invented words in the right one;
    // it is the same mistake with better spelling.
    if (CheckDestination(step.action.control, context) == DestinationVerdict::Mismatch)
    {
        ++stats.wrongDestinations;
        decision.outcome = ContentOutcome::WrongDestination;
        decision.allowed = false;
        decision.detail = "The content was to go in " + task.destination +
            " and this step would put it somewhere else.";
        return decision;
    }

    if (observeDraft && !draftBaseline.valid)
    {
        placement = step.action;
        placement.beforeCommit = {};
        draftBaseline = observeDraft(placement);
        baselineBeforeEntry = draftBaseline.valid;
    }

    const auto refuseMissing = [&](std::string why) {
        ++stats.missingPayloads;
        decision.outcome = ContentOutcome::MissingPayload;
        decision.allowed = false;
        decision.needsInput = true;
        decision.detail = std::move(why);
        return decision;
    };

    if (held.Valid())
    {
        const std::optional<std::string> value =
            vault == nullptr ? std::nullopt : vault->Redeem(held);
        if (!value.has_value())
        {
            // Fails closed. A reference that cannot be redeemed must never become an
            // empty keystroke sequence in a field somebody is about to submit.
            return refuseMissing("The content this task was given is no longer held.");
        }

        const bool wroteOwn = !step.action.value.empty() && !wrotePlaceholder &&
            step.action.value != *value;
        if (wroteOwn)
        {
            decision.attempted = Excerpt(step.action.value);
            if (ResemblesHeld(step.action.value, *value))
            {
                ++stats.modifiedPayloads;
                decision.outcome = ContentOutcome::ModifiedPayload;
                decision.detail =
                    "The step tried to enter an altered copy of the prepared content; "
                    "the original was entered instead.";
            }
            else
            {
                ++stats.inventions;
                decision.outcome = ContentOutcome::InventedText;
                decision.detail =
                    "The step tried to enter text of its own; the prepared content was "
                    "entered instead.";
            }
        }
        else
        {
            ++stats.supplied;
            decision.outcome = ContentOutcome::Supplied;
        }

        // The substitution itself. This is the whole mechanism: whatever any provider
        // proposed, what reaches the machine is the value the runtime is holding, and
        // the typed postcondition is derived from the step afterwards -- so the check
        // that follows asks about the real content and not about a planner's copy of it.
        step.action.value = *value;

        // And the step's description, which the first live run showed matters as much.
        //
        // A planner told to write a fixed token where the words go writes it in
        // `expected` as well -- reasonably, since it has never seen the content and has
        // nothing else to put there. Verification then asks whether the typed condition
        // is *about* the step it is judging, compares "<the prepared content>" against
        // the real value and the real control, finds neither, rules the typed condition
        // irrelevant, and falls back to the substring rule. A step that placed the right
        // words in the right box came back "could not establish".
        //
        // So the description is rewritten to name the destination. The control, never
        // the content: the goal record and the activity feed are exactly the places the
        // user's words are not supposed to reach, and the typed postcondition is already
        // checking the value itself.
        const bool mentionsControl = !step.action.control.empty() &&
            Contains(step.expected, step.action.control);
        if (!mentionsControl || step.expected.find(PreparedContentToken) != std::string::npos)
        {
            step.expected = step.action.control.empty()
                ? std::string("the prepared content is in the field")
                : step.action.control + " holds the prepared content";
        }
        return decision;
    }

    // Drafted, and nothing in custody yet.
    if (task.requirement == ContentRequirement::Drafted)
    {
        if (step.action.value.empty() || wrotePlaceholder)
        {
            return refuseMissing(
                "The step asked to enter prepared content before anything was composed.");
        }
        if (step.action.value.size() > MaximumTaskContentLength || vault == nullptr)
        {
            return refuseMissing("The composed content could not be taken into custody.");
        }
        // Taken into custody at the moment it is chosen, which is what makes a draft
        // checkable. A value still living in a planner's head would be free to come back
        // different on the retry, and the check would then be reading one sentence and
        // the attempt writing another.
        held = vault->Store(step.action.value,
            task.kind.empty() ? "text" : task.kind, ContentProvenance::Drafted);
        ++stats.draftsAdopted;
        decision.outcome = ContentOutcome::DraftAdopted;
        decision.detail = "The composed content is now the task's, and will not change.";
        return decision;
    }

    return refuseMissing(
        "This task needs content that was never supplied, so nothing was typed.");
}

} // namespace revia::computer
