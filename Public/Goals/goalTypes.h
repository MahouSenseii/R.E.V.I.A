#pragma once

#include "Actions/actionTypes.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace revia::goals
{
enum class GoalRecovery
{
    None,
    WaitForState,
    Reobserve,
    NeedVision
};

struct CompletionEvidence
{
    bool accepted = false;
    std::string detail;
};

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
    // The action succeeded but its effect is unknown; retry could duplicate it.
    // Stop for user review rather than treating missing evidence as failure.
    UnverifiedEffect,
    // Requires user input, such as missing exact content; never invents a substitute.
    // Distinct from ambiguity, execution failure, and policy refusal.
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
    // Request-count backstop remains enforceable when the backend omits token usage.
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

// Verified and Failed reflect observed evidence; Unknown means insufficient evidence.
// Unknown must not be treated as failure for retry or as proof of success.
enum class VerificationOutcome
{
    Unknown,
    Verified,
    Failed
};

// Runtime derives typed conditions from action/check pairs, never model output.
// Legacy TextObserved cannot qualify training labels or consequential autonomy.
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
    // Proves text reached the control, not that it was sent, posted, or submitted.
    ControlValueIs,
    // Compares runtime observations before/after a press to prove window change.
    // Supports target-selection evidence only, not that the intended effect occurred.
    ControlStateChanged,
    BrowserControlValueIs
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

    std::string browserSession;
    std::string browserUrl;
    std::uint64_t browserGeneration = 0;

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
    // Descriptive substring result is diagnostic under typed verification, not a gate.
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

// Persisted goals retain their schema during resumption.
// Schema 0 requires typed condition plus expected substring; schema 1 uses the typed
// condition alone when derived, otherwise the legacy substring. Models choose neither.
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
    // Iterative runs must revisit whole-goal acceptance when resumed.
    bool iterative = false;

    std::chrono::system_clock::time_point createdAt = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point updatedAt = std::chrono::system_clock::now();
};

// Derives supported conditions from validated action/check pairs. Unsupported
// shapes use TextObserved; window inspection cannot prove sends, posts, or submissions.
[[nodiscard]] Postcondition DerivePostcondition(const GoalStep& step);

// Canonical readable window state shared by pre-action baselines and post-action checks.
[[nodiscard]] std::string CanonicalWindowState(const actions::ActionResult& result);

[[nodiscard]] VerificationOutcome EvaluatePostcondition(const Postcondition& postcondition, const actions::ActionResult& result);

// Evaluates supplied evidence under the goal's persisted verification contract.
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
[[nodiscard]] VerificationJudgement JudgeStep(std::uint32_t verificationSchema, const Postcondition& postcondition,
    const std::string& expectedText, const actions::ActionResult& result);

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
[[nodiscard]] bool WorthResumingUnprompted(const Goal& goal, std::chrono::system_clock::time_point now);
[[nodiscard]] std::string NewGoalId();
[[nodiscard]] std::string NewStepId();

// Preserves approved roots/applications, forces ApprovedScope, refuses root creation,
// and caps automatic approval at ReversibleWrite. Never widens configured authority.
[[nodiscard]] actions::CapabilitySettings NarrowScopeForGoal(actions::CapabilitySettings configured);

} // namespace revia::goals
