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
    StoreError,
    // The iterative loop kept choosing the same action and the desktop kept not
    // changing. Distinct from a budget: the run was not too expensive, it was stuck,
    // and those want different responses from whoever reads the record.
    NoProgress,
    // The loop asked what to do next and got no usable answer. Also distinct from
    // failure: nothing went wrong, she just could not see a next move.
    Undecided,
    // The action reported success and the check did not confirm it, for an action whose
    // second attempt could happen a second time.
    //
    // Deliberately not VerificationFailed, which says "it did not work". This says "it
    // is not known whether it worked", and the two want opposite responses: the first
    // invites another attempt, the second forbids one. Retrying here is how a message
    // gets sent twice because a receipt went missing.
    UnverifiedEffect,
    // The task needs something from the person before it can go on -- most often exact
    // content it was never given.
    //
    // A fourth distinct outcome, and it has to be. It is not Undecided: nothing is
    // stuck and nothing was ambiguous. It is not Failed: nothing went wrong. It is not
    // Blocked-by-policy: no boundary was reached. It is the runtime declining to
    // supply, from a model, a value that was supposed to come from the user -- which is
    // the alternative to typing a plausible substitute into a field somebody is about
    // to submit.
    NeedsInput
};

// Hard ceilings. The runner stops when any single one is reached; it never
// negotiates a budget upward mid-run.
struct GoalBudget
{
    std::uint32_t maxActions = 20;
    std::uint32_t maxRetriesPerStep = 2;
    std::uint32_t maxTotalRetries = 6;
    std::uint32_t maxTokens = 8192;
    // A ceiling that works when the backend reports nothing.
    //
    // maxTokens can only bound what is measured, and a backend that returns no usage
    // makes every decision free as far as it is concerned. Counting the requests
    // themselves needs no cooperation from anyone, so the run is bounded either way.
    // Set high enough that it is a backstop rather than the thing that normally stops a
    // run -- the action and no-progress bounds are meant to do that first.
    std::uint32_t maxPlannerRequests = 60;
    std::uint64_t maxDurationMs = 120000;
    // How many times in a row the loop may choose the identical action before it is
    // declared stuck. A budget stops work that costs too much; this stops work that
    // costs anything at all and achieves nothing, which a budget alone would let run
    // to exhaustion.
    std::uint32_t maxIdenticalSteps = 3;
};

// What has actually been spent. Compared against GoalBudget after every step.
struct GoalSpend
{
    std::uint32_t actions = 0;
    std::uint32_t retries = 0;
    // Only what a backend actually reported. A decision whose cost came back unknown
    // adds nothing here and is counted below instead, because charging a guess to a
    // ceiling makes the ceiling a guess.
    std::uint32_t tokens = 0;
    std::uint32_t plannerRequests = 0;
    // How many decisions came back with no usage at all. Recorded so "this run cost
    // 400 tokens" can be read as the partial statement it is.
    std::uint32_t unreportedRequests = 0;
    std::uint64_t elapsedMs = 0;
};

// What a check actually established. Three values, because two cannot carry the
// difference that matters.
//
// Verified and Failed are both answers: the evidence was there and it said yes, or the
// evidence was there and it said no. Unknown is the absence of an answer -- the check
// could not run, or it ran and the thing it needed to look at was not in what came
// back. A password field has no readable value; a truncated listing is not a statement
// about what is missing from it.
//
// Collapsing Unknown into Failed is what makes a loop retry an action that already
// happened, and collapsing it into Verified is what makes a goal report success it
// never observed. Both are worse than saying "I could not tell".
enum class VerificationOutcome
{
    Unknown,
    Verified,
    Failed
};

// What success actually looks like, as something that can be checked rather than
// searched for.
//
// TextObserved is what every step had before this existed: the check's output is
// searched for `GoalStep::expected` as a substring. It is retained for compatibility
// and for the steps no derivation covers, and it is weak in ways that matter -- it
// matches text that was already there, it matches a sentence saying the opposite, and
// it matches "Notes-old" when the step created "Notes". A TextObserved result must
// never qualify a training label or newly approved consequential autonomy.
//
// The rest ask a question the evidence can actually answer. They are derived by the
// runtime from the step's own action and check, never read from model output, because
// a postcondition a model wrote for itself is a mark it awards itself.
enum class PostconditionKind
{
    TextObserved,
    // A directory listing contains, or does not contain, an entry with exactly this
    // name. Exactly: the listing is "[FILE]  name", and asking for a substring of that
    // is how "Notes" is satisfied by "MyNotes".
    DirectoryHasEntry,
    DirectoryLacksEntry,
    // A file's contents contain this text. Containment is the real question here, so
    // this one is a substring on purpose.
    FileContains,
    // The window inspected belongs to this executable AND is the one in front. Being
    // able to find a window is not the same as it having focus.
    ForegroundApplicationIs,
    // A named control's current value is exactly this.
    //
    // Note what this does and does not prove. It proves text reached a box. It does not
    // prove the box was sent, posted or submitted -- those are different effects with
    // no evidence here, and a step that performs one of them gets no typed
    // postcondition rather than a flattering one.
    ControlValueIs,
    // The window responded to being pressed.
    //
    // The narrowest honest thing that can be said about a button. A window inspection
    // cannot prove that "Zoom in" zoomed in -- there is no evidence of that anywhere in
    // an accessibility tree -- and that left every press permanently Unknown, which
    // meant no press could ever support a label about anything at all.
    //
    // This asks a smaller question the evidence can actually answer: did the control
    // that was pressed change the state of the window it is in? The runtime takes the
    // same read-only check before the action and after it and compares what came back.
    // Both observations are its own, so nothing grades itself, and the cost is one
    // extra read-only inspection on the steps that use it.
    //
    // What it establishes is that the target was real, reachable, and did something --
    // which is a fact about *target selection*. It says nothing about whether what it
    // did was what was wanted: pressing Delete and pressing Save both change a window.
    // Anything built on this has to keep those two apart.
    ControlStateChanged
};

struct Postcondition
{
    PostconditionKind kind = PostconditionKind::TextObserved;
    // The control, file or application the condition is about. Unused by TextObserved
    // and FileContains, which ask about the whole result.
    std::string subject;
    // What it should be. For TextObserved this is GoalStep::expected, carried here so
    // evaluation has one input rather than two.
    std::string value;

    // Whether this condition can be trusted to have said "no" rather than "I could not
    // tell". Only a derived condition can: TextObserved failing to find a substring is
    // as consistent with a truncated listing as with the work not happening.
    [[nodiscard]] bool IsTyped() const { return kind != PostconditionKind::TextObserved; }
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
    // Kept as the plain "did this step succeed" flag every existing reader uses. It is
    // `outcome == Verified` and nothing else, so the two can never disagree.
    bool verified = false;
    // What the check established, and by which question. Recorded rather than derived
    // afterwards, because whether a past run could tell is not recoverable from a
    // boolean.
    VerificationOutcome outcome = VerificationOutcome::Unknown;
    PostconditionKind checkedBy = PostconditionKind::TextObserved;
    // What the step's descriptive `expected` text would have concluded on its own.
    //
    // Under the typed contract it is a diagnostic and not a gate: a deletion's evidence
    // is a listing the deleted name is absent from, so demanding the name be found
    // there contradicts the very thing being proved. Recorded anyway, because a typed
    // pass whose description disagrees is worth seeing in the record even when it is
    // not worth failing the step over.
    VerificationOutcome expectedTextSeen = VerificationOutcome::Unknown;
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
    // Filled by the runner from the action and check above, immediately before the step
    // runs. Not part of what a planner may write.
    Postcondition postcondition;

    std::vector<StepAttempt> attempts;
};

// Which verification contract a goal's steps are judged under.
//
// Versioned because the rule changed and a goal persisted by an older build must keep
// being read the way it was written. A resumed goal carries its own schema; it is never
// upgraded underneath a run.
//
// 0 -- LegacyCombined. A typed postcondition AND the step's descriptive `expected`
//      substring both had to hold. Correct for the positive shapes and self-
//      contradictory for the negative one: proving a file is gone meant finding its
//      name in a listing it is gone from, so a deletion that worked was recorded as an
//      effect that could not be verified (ISSUE-REVIA-0069).
// 1 -- TypedContract. Where the runtime derived a typed postcondition, that condition
//      is the contract and is evaluated alone. The descriptive text is kept for display
//      and diagnosis. Where no typed condition was derived, the legacy substring rule
//      is still the whole rule, so every shape the derivation does not recognise is
//      judged exactly as it was before.
//
// What this does not do is let anything outside the runtime choose the question. The
// postcondition is still derived from the step's own validated action by
// DerivePostcondition and never parsed from a model's output, so making it the contract
// hands no grading authority to the thing being graded.
inline constexpr std::uint32_t LegacyCombinedVerification = 0;
inline constexpr std::uint32_t TypedContractVerification = 1;
inline constexpr std::uint32_t CurrentVerificationSchema = TypedContractVerification;

struct Goal
{
    std::string id;
    std::string title;
    GoalStatus status = GoalStatus::Planned;
    StopReason stopReason = StopReason::None;
    // Bounded explanation from the planner or validator; data, never action authority.
    std::string stopDetail;

    std::vector<GoalStep> steps;
    std::uint32_t currentStep = 0;

    GoalBudget budget;
    GoalSpend spend;

    // Per-goal capability scope, narrower than the global profile. This feeds a
    // CapabilityPolicy directly, so a goal can never widen its own authority.
    actions::CapabilitySettings scope;

    // The contract this goal's steps are judged under. New goals get the current one;
    // a goal read back from an older database keeps the one it was written with.
    std::uint32_t verificationSchema = CurrentVerificationSchema;

    std::chrono::system_clock::time_point createdAt = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point updatedAt = std::chrono::system_clock::now();
};

// What the step's own action and check make it possible to prove.
//
// Derived, never parsed. A postcondition supplied from outside would be the step
// grading itself, and the whole point of a check is that it is not the actor's word.
// The derivation is deterministic and knows only a handful of shapes; anything it does
// not recognise falls back to TextObserved with `expected`, which is exactly the
// behaviour every step had before.
//
// The shapes it knows are the ones where the action says precisely what the check
// should find: a directory that must now contain what was created or moved into it, a
// directory that must no longer contain what was recycled, a window that must belong
// to the application that was launched, an edit control that must hold the text that
// was typed into it.
//
// Sending, posting and submitting are deliberately absent. There is no evidence in a
// window inspection that a message left the machine, so a step that sends one gets no
// typed postcondition -- which leaves it Unknown rather than giving it a check it
// would pass for the wrong reason.
[[nodiscard]] Postcondition DerivePostcondition(const GoalStep& step);

// Whether the check actually established the postcondition, from what the check
// returned. Pure, so the question "would this evidence have proved it?" is answerable
// in a test with no desktop and no filesystem.
// The window's readable state, as one comparable string.
//
// Used by the runner to take a baseline before a press and by the evaluation to compare
// against it afterwards. One function, deliberately, because two spellings of "the
// state" is how a comparison starts reporting changes that are really formatting.
[[nodiscard]] std::string CanonicalWindowState(const actions::ActionResult& result);

[[nodiscard]] VerificationOutcome EvaluatePostcondition(
    const Postcondition& postcondition, const actions::ActionResult& result);

// What a step's evidence established, under the goal's own verification contract.
//
// Pure, so "would this evidence have proved it?" stays answerable in a test with no
// desktop and no filesystem -- including the question this type exists for, which is
// whether the two contracts disagree about the same evidence.
struct VerificationJudgement
{
    VerificationOutcome outcome = VerificationOutcome::Unknown;
    // What the descriptive text alone would have concluded. A diagnostic under the
    // typed contract, and half the rule under the legacy one.
    VerificationOutcome expectedTextSeen = VerificationOutcome::Unknown;
    // Which question actually answered it.
    PostconditionKind checkedBy = PostconditionKind::TextObserved;
    // Whether the typed condition was about the subject the step named. False for a
    // step with no typed condition, and false for one whose action and description
    // disagree -- which is the case where a typed pass proves nothing about the step.
    bool typedIsRelevant = false;
};

// Judge one step's check result.
//
// `verificationSchema` is the goal's, not the build's: a goal written under the legacy
// contract is judged under the legacy contract however new the code reading it is.
[[nodiscard]] VerificationJudgement JudgeStep(
    std::uint32_t verificationSchema,
    const Postcondition& postcondition,
    const std::string& expectedText,
    const actions::ActionResult& result);

[[nodiscard]] std::string ToString(VerificationOutcome value);
[[nodiscard]] VerificationOutcome VerificationOutcomeFromString(const std::string& value);
[[nodiscard]] std::string ToString(PostconditionKind value);
[[nodiscard]] PostconditionKind PostconditionKindFromString(const std::string& value);
[[nodiscard]] std::string ToString(GoalStatus value);
[[nodiscard]] std::string ToString(StepStatus value);
[[nodiscard]] std::string ToString(StopReason value);
[[nodiscard]] GoalStatus GoalStatusFromString(const std::string& value);
[[nodiscard]] StepStatus StepStatusFromString(const std::string& value);
[[nodiscard]] StopReason StopReasonFromString(const std::string& value);
[[nodiscard]] bool IsTerminal(GoalStatus value);
// Whether Revia should bring this goal up, or pick it back up, without being asked. Not
// when policy stopped it: only the user can change what stopped it. Not when it has sat
// untouched for a day: by then it has been left, not interrupted. Both stay listed under
// /goals and can be resumed on request.
[[nodiscard]] bool WorthResumingUnprompted(
    const Goal& goal, std::chrono::system_clock::time_point now);
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
