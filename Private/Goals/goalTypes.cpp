#include "Goals/goalTypes.h"

#include <algorithm>
#include <filesystem>
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
        case StopReason::InvalidPlan: return "invalid_plan";
        case StopReason::StoreError: return "store_error";
        case StopReason::NoProgress: return "no_progress";
        case StopReason::Undecided: return "undecided";
        case StopReason::UnverifiedEffect: return "unverified_effect";
        case StopReason::NeedsInput: return "needs_input";
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
    if (name == "invalid_plan") return StopReason::InvalidPlan;
    if (name == "store_error") return StopReason::StoreError;
    if (name == "no_progress") return StopReason::NoProgress;
    if (name == "undecided") return StopReason::Undecided;
    if (name == "unverified_effect") return StopReason::UnverifiedEffect;
    if (name == "needs_input") return StopReason::NeedsInput;
    return StopReason::None;
}

bool WorthResumingUnprompted(
    const Goal& goal, const std::chrono::system_clock::time_point now)
{
    return !IsTerminal(goal.status) && !goal.steps.empty() &&
        goal.stopReason != StopReason::PolicyBlocked &&
        now - goal.updatedAt < std::chrono::hours{24};
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

std::string ToString(const VerificationOutcome value)
{
    switch (value)
    {
        case VerificationOutcome::Verified: return "verified";
        case VerificationOutcome::Failed: return "failed";
        case VerificationOutcome::Unknown:
        default: return "unknown";
    }
}

// Unknown is the fail-closed answer here, and deliberately so: a stored outcome that
// cannot be read is not evidence of anything, least of all of success.
VerificationOutcome VerificationOutcomeFromString(const std::string& value)
{
    const std::string name = NormalizeName(value);
    if (name == "verified") return VerificationOutcome::Verified;
    if (name == "failed") return VerificationOutcome::Failed;
    return VerificationOutcome::Unknown;
}

std::string ToString(const PostconditionKind value)
{
    switch (value)
    {
        case PostconditionKind::DirectoryHasEntry: return "directory_has_entry";
        case PostconditionKind::DirectoryLacksEntry: return "directory_lacks_entry";
        case PostconditionKind::FileContains: return "file_contains";
        case PostconditionKind::ForegroundApplicationIs: return "foreground_application_is";
        case PostconditionKind::ControlValueIs: return "control_value_is";
        case PostconditionKind::ControlStateChanged: return "control_state_changed";
        case PostconditionKind::TextObserved:
        default: return "text_observed";
    }
}

// An unreadable kind falls back to the weak one rather than to a strong one it cannot
// justify. A stored row claiming a typed check it may not have run would let a past
// result qualify for something the typed kinds exist to gate.
PostconditionKind PostconditionKindFromString(const std::string& value)
{
    const std::string name = NormalizeName(value);
    if (name == "directory_has_entry") return PostconditionKind::DirectoryHasEntry;
    if (name == "directory_lacks_entry") return PostconditionKind::DirectoryLacksEntry;
    if (name == "file_contains") return PostconditionKind::FileContains;
    if (name == "foreground_application_is") return PostconditionKind::ForegroundApplicationIs;
    if (name == "control_value_is") return PostconditionKind::ControlValueIs;
    if (name == "control_state_changed") return PostconditionKind::ControlStateChanged;
    return PostconditionKind::TextObserved;
}

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

std::string Trimmed(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// A directory listing entry is "[FILE]  name" or "[DIR]   name". The name is what a
// postcondition is about, so the prefix comes off before anything is compared. The
// alternative is asking whether "Notes" appears in "[FILE]  MyNotes.txt", which it
// does.
std::string EntryName(const std::string& entry)
{
    if (!entry.empty() && entry.front() == '[')
    {
        const auto close = entry.find(']');
        if (close != std::string::npos)
        {
            return Trimmed(entry.substr(close + 1));
        }
    }
    return Trimmed(entry);
}

// InspectWindow writes "Application: x\nWindow: y\nForeground: true". Reads one
// labelled line and returns empty when the label is absent, which the caller treats as
// Unknown rather than as a mismatch.
std::string LabelledLine(const std::string& content, const std::string& label)
{
    const std::string needle = label + ": ";
    std::size_t at = content.find(needle);
    while (at != std::string::npos)
    {
        if (at == 0 || content[at - 1] == '\n')
        {
            const std::size_t from = at + needle.size();
            const std::size_t end = content.find('\n', from);
            return Trimmed(content.substr(from, end == std::string::npos
                ? std::string::npos : end - from));
        }
        at = content.find(needle, at + 1);
    }
    return {};
}

// The window's readable state, as one comparable string.
//
// Built from the check's own parsed control lines rather than from its formatted text,
// so a difference in ordering or in the summary header is not mistaken for a change on
// screen. Only the lines are used, and they are used whole: a control appearing,
// disappearing, becoming enabled, taking focus or changing value all count, because all
// of them are the window doing something.
//
// Exposed here rather than inlined so that the before and after are, provably, the same
// function of the same kind of input. Two spellings of "the state" is how a comparison
// starts reporting changes that are really formatting.
std::string CanonicalWindowStateImpl(const actions::ActionResult& result)
{
    std::vector<std::string> lines;
    lines.reserve(result.entries.size());
    for (const std::string& entry : result.entries)
    {
        lines.push_back(Trimmed(entry));
    }
    // Sorted, because the order controls are enumerated in is not a fact about the
    // window. A tree walk that returns siblings in a different order twice running
    // would otherwise read as the window having changed.
    std::sort(lines.begin(), lines.end());
    std::string canonical;
    for (const std::string& line : lines)
    {
        canonical += line;
        canonical += '\n';
    }
    return canonical;
}

// A control line is "<name> [type=N, enabled=true, id=X, focused=true, value=V]".
bool ControlLineMatches(const std::string& entry, const std::string& subject)
{
    const auto open = entry.find(" [");
    const std::string name = Trimmed(open == std::string::npos
        ? entry : entry.substr(0, open));
    if (Lowered(name) == Lowered(subject)) return true;
    const auto id = entry.find(", id=");
    if (id == std::string::npos) return false;
    const std::size_t from = id + 5;
    const std::size_t end = entry.find_first_of(",]", from);
    return Lowered(Trimmed(entry.substr(from, end == std::string::npos
        ? std::string::npos : end - from))) == Lowered(subject);
}

// The value runs to the closing bracket, because a typed value may itself contain
// commas.
bool ControlValue(const std::string& entry, std::string& outValue)
{
    const auto at = entry.find(", value=");
    if (at == std::string::npos) return false;
    const std::size_t from = at + 8;
    const std::size_t close = entry.rfind(']');
    if (close == std::string::npos || close < from) return false;
    outValue = entry.substr(from, close - from);
    return true;
}

} // namespace

std::string CanonicalWindowState(const actions::ActionResult& result)
{
    return CanonicalWindowStateImpl(result);
}

VerificationOutcome EvaluatePostcondition(
    const Postcondition& postcondition, const actions::ActionResult& result)
{
    // A check that did not run establishes nothing. This is the commonest source of
    // Unknown and the reason the tri-state exists: "the check failed" and "the check
    // says no" are opposite facts.
    if (!result.succeeded)
    {
        return VerificationOutcome::Unknown;
    }

    switch (postcondition.kind)
    {
        case PostconditionKind::DirectoryHasEntry:
        case PostconditionKind::DirectoryLacksEntry:
        {
            if (postcondition.value.empty()) return VerificationOutcome::Unknown;
            const bool present = std::any_of(result.entries.begin(), result.entries.end(),
                [&](const std::string& entry)
                {
                    return EntryName(entry) == postcondition.value;
                });
            const bool wanted = postcondition.kind == PostconditionKind::DirectoryHasEntry;
            return present == wanted
                ? VerificationOutcome::Verified : VerificationOutcome::Failed;
        }

        case PostconditionKind::FileContains:
        {
            if (postcondition.value.empty()) return VerificationOutcome::Unknown;
            return result.content.find(postcondition.value) != std::string::npos
                ? VerificationOutcome::Verified : VerificationOutcome::Failed;
        }

        case PostconditionKind::ForegroundApplicationIs:
        {
            const std::string application = LabelledLine(result.content, "Application");
            const std::string foreground = LabelledLine(result.content, "Foreground");
            // No application line means the inspection did not report one, not that it
            // reported the wrong one.
            if (application.empty() || postcondition.value.empty())
            {
                return VerificationOutcome::Unknown;
            }
            if (Lowered(application) != Lowered(postcondition.value))
            {
                return VerificationOutcome::Failed;
            }
            // Finding the window is not the same as its being in front, and the absence
            // of the line is not evidence that it is not.
            if (foreground.empty()) return VerificationOutcome::Unknown;
            return foreground == "true"
                ? VerificationOutcome::Verified : VerificationOutcome::Failed;
        }

        case PostconditionKind::ControlValueIs:
        {
            if (postcondition.subject.empty()) return VerificationOutcome::Unknown;
            for (const std::string& entry : result.entries)
            {
                if (!ControlLineMatches(entry, postcondition.subject)) continue;
                std::string value;
                // The control is there but its value was not reported: a password
                // field, or one with no value pattern. That is the clearest Unknown
                // there is, because something is deliberately not readable.
                if (!ControlValue(entry, value)) return VerificationOutcome::Unknown;
                return value == postcondition.value
                    ? VerificationOutcome::Verified : VerificationOutcome::Failed;
            }
            // The control was not in what came back. The listing is capped, so absence
            // here is not absence on screen.
            return VerificationOutcome::Unknown;
        }

        case PostconditionKind::ControlStateChanged:
        {
            // No baseline means the comparison was never set up, which is an absence of
            // evidence and not evidence of absence.
            if (postcondition.value.empty()) return VerificationOutcome::Unknown;
            const std::string after = CanonicalWindowStateImpl(result);
            if (after.empty()) return VerificationOutcome::Unknown;
            // Different means the control did something. Identical means the window is
            // exactly as it was, which for a press is a real answer: it did nothing
            // observable, so pressing it again would do nothing observable either.
            return after != postcondition.value
                ? VerificationOutcome::Verified : VerificationOutcome::Failed;
        }

        case PostconditionKind::TextObserved:
        default:
        {
            // The original rule, unchanged, and still weak in the ways the header
            // describes. It reports Verified or Unknown and never Failed: not finding a
            // substring is as consistent with a truncated result as with the work not
            // happening, and callers use Failed to decide that repeating an action is
            // safe.
            if (postcondition.value.empty()) return VerificationOutcome::Unknown;
            const std::string needle = Lowered(postcondition.value);
            if (Lowered(result.content).find(needle) != std::string::npos)
            {
                return VerificationOutcome::Verified;
            }
            for (const std::string& entry : result.entries)
            {
                if (Lowered(entry).find(needle) != std::string::npos)
                {
                    return VerificationOutcome::Verified;
                }
            }
            return VerificationOutcome::Unknown;
        }
    }
}

namespace
{

// Whether a typed postcondition is about the thing the step said it was about.
//
// The subject of the claim -- the entry name, the application, the text that went into
// the control -- has to appear in the step's own description of success. That is
// corroboration from a second source: the condition comes from the action's parameters
// and the description comes from whoever proposed the step, and a step whose two halves
// disagree has not established anything about its stated effect no matter how cleanly
// its action ran.
//
// FileContains is relevant by construction: it is derived *from* the expected text, so
// the two cannot disagree.
[[nodiscard]] bool PostconditionDescribes(
    const Postcondition& postcondition, const std::string& expectedText)
{
    const std::string described = Lowered(expectedText);
    const auto mentions = [&](const std::string& subject)
    {
        if (subject.empty()) return false;
        return described.find(Lowered(subject)) != std::string::npos;
    };

    switch (postcondition.kind)
    {
        case PostconditionKind::FileContains:
            return true;
        case PostconditionKind::DirectoryHasEntry:
        case PostconditionKind::DirectoryLacksEntry:
        case PostconditionKind::ForegroundApplicationIs:
            return mentions(postcondition.value);
        case PostconditionKind::ControlValueIs:
            // Either half corroborates: a step may name the box it filled or the text
            // it put there, and naming one of them ties the description to the action.
            return mentions(postcondition.value) || mentions(postcondition.subject);
        case PostconditionKind::ControlStateChanged:
            // Only the subject. The value here is a snapshot of the whole window, which
            // no step description could sensibly mention -- asking it to would make the
            // condition permanently irrelevant and quietly send every press back to the
            // substring rule, which is exactly what happened the first time this kind
            // was added without touching this function.
            return mentions(postcondition.subject);
        case PostconditionKind::TextObserved:
        default:
            return false;
    }
}

} // namespace

VerificationJudgement JudgeStep(
    const std::uint32_t verificationSchema,
    const Postcondition& postcondition,
    const std::string& expectedText,
    const actions::ActionResult& result)
{
    VerificationJudgement judgement;
    judgement.checkedBy = postcondition.kind;

    // The descriptive text, asked on its own. Always computed: under the legacy
    // contract it is half the rule, and under the typed one it is what the record shows
    // a reader who wants to know whether the step did what it said it would.
    Postcondition described;
    described.kind = PostconditionKind::TextObserved;
    described.value = expectedText;
    judgement.expectedTextSeen = EvaluatePostcondition(described, result);

    if (!postcondition.IsTyped())
    {
        // Nothing was derived for this shape. The legacy rule is the whole rule, which
        // is where every step stood before any of this existed, and where every step
        // the derivation does not recognise still stands.
        judgement.outcome = judgement.expectedTextSeen;
        return judgement;
    }

    const VerificationOutcome typed = EvaluatePostcondition(postcondition, result);
    judgement.typedIsRelevant = PostconditionDescribes(postcondition, expectedText);

    if (verificationSchema < TypedContractVerification)
    {
        // The original combined rule, preserved exactly for goals written under it.
        //
        // Failed comes only from the typed condition, because only a typed one can tell
        // "no" from "I could not tell", and callers use Failed to decide that repeating
        // a committing action is safe.
        if (typed == VerificationOutcome::Failed)
        {
            judgement.outcome = VerificationOutcome::Failed;
        }
        else if (typed == VerificationOutcome::Verified)
        {
            judgement.outcome = judgement.expectedTextSeen == VerificationOutcome::Verified
                ? VerificationOutcome::Verified : VerificationOutcome::Unknown;
        }
        else
        {
            judgement.outcome = judgement.expectedTextSeen;
        }
        return judgement;
    }

    // The typed contract, and the one condition on it.
    //
    // A typed postcondition is only the contract when it is *about the step*. It is
    // derived from the action's own parameters, so it always faithfully describes what
    // the action did -- which is not the same as describing what the step said it would
    // do. A step announcing one thing and acting on another would otherwise grade
    // itself on the thing it acted on and pass, which is "I created something, so
    // something must be right".
    //
    // Relevance is that corroboration: the step's own description names the subject the
    // typed condition is about. Deliberately a weak test used for a strong purpose --
    // it only chooses which contract applies, and choosing wrong in either direction
    // degrades toward the older, stricter reading rather than away from it.
    if (!judgement.typedIsRelevant)
    {
        // The condition does not answer the question the step asked. A typed "no" is
        // still worth carrying, because it is the only thing that establishes an action
        // did not land and therefore that repeating it is safe; but a typed "yes" about
        // something else proves nothing about this step, and the descriptive text is
        // all that is left.
        judgement.outcome = typed == VerificationOutcome::Failed
            ? VerificationOutcome::Failed : judgement.expectedTextSeen;
        return judgement;
    }

    // The condition the runtime derived is the question, and it is asked alone.
    //
    // The text is not asked again as a second requirement because for a negative
    // condition the two questions contradict each other: DirectoryLacksEntry is proved
    // by a listing the name is absent from, and the substring rule wants that same name
    // found in that same listing. Under the old rule a deletion that demonstrably
    // worked came back Unknown and stopped the run as an unverified effect
    // (ISSUE-REVIA-0069).
    //
    // This is not the step grading itself. DerivePostcondition reads the action's own
    // parameters -- the path it recycled, the control it typed into, the application it
    // launched -- and never model output, so the question is the runtime's throughout.
    judgement.outcome = typed;
    return judgement;
}

Postcondition DerivePostcondition(const GoalStep& step)
{
    using actions::ActionType;
    Postcondition legacy;
    legacy.kind = PostconditionKind::TextObserved;
    legacy.value = step.expected;

    const auto filename = [](const std::filesystem::path& path)
    {
        return actions::PathToUtf8(path.filename());
    };

    if (step.check.type == ActionType::ListDirectory)
    {
        Postcondition derived;
        switch (step.action.type)
        {
            case ActionType::CreateDirectory:
                derived.kind = PostconditionKind::DirectoryHasEntry;
                derived.value = filename(step.action.source);
                break;
            case ActionType::CopyFile:
            case ActionType::MoveFile:
            case ActionType::RenamePath:
                derived.kind = PostconditionKind::DirectoryHasEntry;
                derived.value = filename(step.action.destination);
                break;
            case ActionType::MoveToRecycleBin:
                derived.kind = PostconditionKind::DirectoryLacksEntry;
                derived.value = filename(step.action.source);
                break;
            default:
                return legacy;
        }
        return derived.value.empty() ? legacy : derived;
    }

    if (step.check.type == ActionType::ReadTextFile &&
        step.action.type == ActionType::ReadTextFile && !step.expected.empty())
    {
        Postcondition derived;
        derived.kind = PostconditionKind::FileContains;
        derived.value = step.expected;
        return derived;
    }

    if (step.check.type == ActionType::InspectWindow)
    {
        // Typing into a named control, verified by reading that control back. This is
        // the one that separates composing from sending: it proves the text is in the
        // box and says nothing at all about whether the box was submitted.
        if ((step.action.type == ActionType::SetControlText ||
                step.action.type == ActionType::TypeText) &&
            !step.action.control.empty() && !step.action.value.empty())
        {
            Postcondition derived;
            derived.kind = PostconditionKind::ControlValueIs;
            derived.subject = step.action.control;
            derived.value = step.action.value;
            return derived;
        }
        // A press, checked by inspecting the window it was pressed in.
        //
        // The value is deliberately left empty here. It is the *prior* state of the
        // window, which is not a property of the step and cannot be derived from one:
        // the runner fills it by taking the same read-only check once before acting.
        // An empty value evaluates to Unknown, so a step that somehow reaches
        // verification without a baseline says "I could not tell" rather than passing.
        if (step.action.type == ActionType::InvokeControl &&
            !step.action.control.empty() && !step.action.application.empty())
        {
            Postcondition derived;
            derived.kind = PostconditionKind::ControlStateChanged;
            derived.subject = step.action.control;
            return derived;
        }
        if ((step.action.type == ActionType::LaunchApplication ||
                step.action.type == ActionType::FocusWindow) &&
            !step.action.application.empty())
        {
            Postcondition derived;
            derived.kind = PostconditionKind::ForegroundApplicationIs;
            derived.value = step.action.application;
            return derived;
        }
    }

    return legacy;
}

actions::CapabilitySettings NarrowScopeForGoal(actions::CapabilitySettings configured)
{
    configured.mode = actions::ExecutionMode::ApprovedScope;
    configured.createMissingApprovedRoots = false;
    if (configured.autoApproveRiskThrough > actions::RiskLevel::ReversibleWrite)
    {
        configured.autoApproveRiskThrough = actions::RiskLevel::ReversibleWrite;
    }
    return configured;
}

} // namespace revia::goals
