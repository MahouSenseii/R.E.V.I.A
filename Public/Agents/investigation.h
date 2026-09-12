#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::agents
{

// One investigation, scoped to one task.
//
// This extends the single self-questioning pass into a loop that can learn. The existing
// SelfInquiryAgent asks a set of questions once and hands them to the answer; what it
// cannot do is look at what came back and ask a *better* question because of it. That is
// the whole of the difference, and it is the only thing worth building here: printing
// more questions before giving the same answer would be a decoration, not an
// investigation.
//
// Two distinctions are load-bearing throughout and are enforced by the types rather than
// by convention:
//
//   - A model's answer to a question is not evidence. `EvidenceKind` separates what a
//     tool observed from what the model concluded, because a loop that treats its own
//     output as a finding will confirm anything it starts out believing.
//   - A question the runtime never checked is Unresolved or Blocked, never Supported.
//     Running out of budget is a pause, not a result.

enum class QuestionStatus
{
    // Raised, not yet investigated.
    Open,
    // Selected for the current round.
    Checking,
    // A check produced evidence for it.
    Supported,
    // A check produced evidence against it.
    Refuted,
    // Checked, and the evidence did not settle it.
    Unresolved,
    // Could not be checked: the evidence was unavailable or permission was refused.
    Blocked
};

[[nodiscard]] std::string ToString(QuestionStatus status);
// True once a question no longer needs the loop's attention.
[[nodiscard]] bool IsSettled(QuestionStatus status);

// Where something came from. The separation exists so that "the model said so" can never
// be silently promoted to "this was observed".
enum class EvidenceKind
{
    // A tool, file, log, measurement, test, or calculation produced this.
    ToolObservation,
    // The model's reading of something. Useful, and not the same as having checked.
    ModelInterpretation
};

[[nodiscard]] std::string ToString(EvidenceKind kind);

// What kind of check was proposed. Recorded so a finding can say which check produced it,
// and so "no tool was actually run" is visible rather than implied.
enum class CheckKind
{
    ResolvedConfiguration,
    SourceCode,
    LogsAndMeasurements,
    Tests,
    FileOrApplicationState,
    Research,
    Calculation,
    // The model reasoned about it without consulting anything. Permitted, and never
    // counted as an observation.
    ModelReasoning
};

[[nodiscard]] std::string ToString(CheckKind kind);
// Whether this kind of check, when it completes, yields a tool observation.
[[nodiscard]] bool ProducesObservation(CheckKind kind);

struct InvestigationQuestion
{
    std::string id;
    std::string text;
    QuestionStatus status = QuestionStatus::Open;
    // The round that raised it. Round 0 means it came from the original goal.
    std::size_t raisedInRound = 0;
    std::size_t checkedInRound = 0;
    // Questions that must settle before this one can be checked.
    std::vector<std::string> dependsOn;
    // How much the answer could change the outcome, 0..1. Drives selection order:
    // the point is to ask what could change the answer, not what is easy to ask.
    double materiality = 0.5;
    // Findings that arose because of this question.
    std::vector<std::string> findingIds;
    // Set when a later round reopened it, with the evidence that justified reopening.
    std::string reopenedBecause;

    // Case- and punctuation-insensitive form, for detecting a question already asked.
    [[nodiscard]] std::string Normalized() const;
};

struct InvestigationCheck
{
    std::string id;
    std::string questionId;
    std::string description;
    CheckKind kind = CheckKind::ModelReasoning;
    bool attempted = false;
    bool completed = false;
    // Populated when an existing permission, approval, or policy path refused it. A
    // self-generated question grants no authority, so this is an ordinary outcome.
    std::string refusal;
};

struct Finding
{
    std::string id;
    std::string questionId;
    // Which check produced it. Never empty for a recorded finding: a finding with no
    // check behind it is an assertion.
    std::string checkId;
    EvidenceKind kind = EvidenceKind::ModelInterpretation;
    // What was observed, in one or two lines.
    std::string observed;
    std::vector<std::string> supportsHypotheses;
    std::vector<std::string> contradictsHypotheses;
    // What this does not establish. Recorded because a finding without its limits reads
    // as more than it is.
    std::string limitations;
    std::size_t round = 0;
};

struct Hypothesis
{
    enum class Standing
    {
        Proposed,
        Supported,
        // Kept, not deleted. The contradiction is part of what was learned, and erasing
        // it is how a loop ends up re-proposing the same wrong idea three rounds later.
        Contradicted,
        Abandoned
    };

    std::string id;
    std::string text;
    Standing standing = Standing::Proposed;
    std::vector<std::string> supportedBy;
    std::vector<std::string> contradictedBy;
};

[[nodiscard]] std::string ToString(Hypothesis::Standing standing);

// Configurable bounds. Exhausting any of them pauses the investigation; none of them
// makes it succeed.
struct InvestigationBudget
{
    std::chrono::milliseconds wallClock{45000};
    std::size_t maximumRounds = 5;
    std::size_t maximumToolCalls = 24;
    std::size_t maximumTokens = 40000;
    // Questions allowed to be outstanding at once, so the loop cannot fan out forever.
    std::size_t maximumPendingQuestions = 8;
    // Rounds that may pass without new evidence or a new question before the loop stops.
    std::size_t maximumNoProgressRounds = 2;
    // Questions that may be checked in one round.
    std::size_t maximumQuestionsPerRound = 3;
};

enum class InvestigationOutcome
{
    Running,
    // The goal was addressed and the runtime's completion checks passed.
    Completed,
    // A bound was reached. Explicitly not a success.
    Paused,
    // Nothing further can be checked: the evidence is unavailable.
    Blocked,
    Cancelled
};

[[nodiscard]] std::string ToString(InvestigationOutcome outcome);

// The task-scoped state. One per investigation, owned by the turn that started it.
class Investigation
{
public:
    Investigation() = default;
    Investigation(std::uint64_t taskId, std::string goal, std::string completionCriteria);

    [[nodiscard]] std::uint64_t TaskId() const { return taskId; }
    [[nodiscard]] const std::string& Goal() const { return goal; }
    [[nodiscard]] const std::string& CompletionCriteria() const { return completionCriteria; }

    // Adds a question unless an equivalent one is already present.
    //
    // `justification` is required to reopen something already settled; without it an
    // equivalent question is refused and the refusal is what the no-progress detector
    // counts. Returns the id, or empty when refused.
    std::string AddQuestion(
        const std::string& text,
        double materiality,
        std::size_t round,
        std::vector<std::string> dependsOn = {},
        const std::string& justification = {});

    std::string AddHypothesis(const std::string& text);
    std::string RecordCheck(InvestigationCheck check);
    std::string RecordFinding(Finding finding);

    void SetQuestionStatus(const std::string& questionId, QuestionStatus status);
    // Records which round a question was checked in, so the transcript can group its
    // summary under the right round.
    void MarkChecked(const std::string& questionId, std::size_t round);
    void SetHypothesisStanding(const std::string& id, Hypothesis::Standing standing);

    // Questions that could be checked now: Open, and with every dependency settled.
    [[nodiscard]] std::vector<InvestigationQuestion> Selectable() const;
    // Highest-materiality selectable questions, at most `limit`. Ties break toward the
    // question raised earliest, so the loop finishes what it started.
    [[nodiscard]] std::vector<InvestigationQuestion> SelectForRound(std::size_t limit) const;

    [[nodiscard]] const std::vector<InvestigationQuestion>& Questions() const { return questions; }
    [[nodiscard]] const std::vector<Finding>& Findings() const { return findings; }
    [[nodiscard]] const std::vector<InvestigationCheck>& Checks() const { return checks; }
    [[nodiscard]] const std::vector<Hypothesis>& Hypotheses() const { return hypotheses; }

    [[nodiscard]] const InvestigationQuestion* FindQuestion(const std::string& id) const;
    [[nodiscard]] std::size_t PendingCount() const;
    [[nodiscard]] std::size_t ObservationCount() const;
    // Questions that are unsettled and material enough that finishing without them would
    // be finishing early.
    [[nodiscard]] std::vector<std::string> MaterialOpenQuestions(double threshold) const;
    // Hypotheses contradicted by evidence and not yet abandoned or replaced.
    [[nodiscard]] std::vector<std::string> UnresolvedContradictions() const;
    [[nodiscard]] std::vector<std::string> Blockers() const;

    // A short, task-relevant summary of one round, for the transcript. Deliberately not a
    // dump of everything the model produced.
    [[nodiscard]] std::string CheckingSummary(std::size_t round) const;
    [[nodiscard]] std::string FindingsSummary(std::size_t round) const;
    // The expandable detail behind a round: which check produced what, and its limits.
    [[nodiscard]] std::string EvidenceDetail(std::size_t round) const;
    // Everything learned, for the final answer's prompt block.
    [[nodiscard]] std::string PromptBlock() const;

private:
    [[nodiscard]] bool HasEquivalentQuestion(
        const std::string& normalized, const InvestigationQuestion** outExisting) const;

    std::uint64_t taskId = 0;
    std::string goal;
    std::string completionCriteria;
    std::vector<InvestigationQuestion> questions;
    std::vector<InvestigationCheck> checks;
    std::vector<Finding> findings;
    std::vector<Hypothesis> hypotheses;
    std::uint64_t nextId = 1;
};

// ---------------------------------------------------------------- the loop

// What the loop hands an investigator for one round.
struct RoundRequest
{
    std::uint64_t taskId = 0;
    std::size_t round = 0;
    std::string goal;
    // The questions to check now.
    std::vector<InvestigationQuestion> questions;
    // Everything already observed. This is what makes the next question better than the
    // last: the investigator is handed the actual results, not a summary of its own
    // earlier guesses.
    std::vector<Finding> priorFindings;
    std::vector<Hypothesis> hypotheses;
    // What remains of the budget, so an investigator can prefer the smallest useful check.
    std::size_t toolCallsRemaining = 0;
    std::chrono::milliseconds timeRemaining{0};
};

// A question raised by what this round found.
struct ProposedQuestion
{
    std::string text;
    double materiality = 0.5;
    std::vector<std::string> dependsOn;
    // Required when reopening something equivalent to a settled question.
    std::string justification;
};

struct CheckOutcome
{
    std::string questionId;
    QuestionStatus status = QuestionStatus::Unresolved;
    CheckKind checkKind = CheckKind::ModelReasoning;
    std::string checkDescription;
    std::string observed;
    std::string limitations;
    std::vector<std::string> supportsHypotheses;
    std::vector<std::string> contradictsHypotheses;
    // Set when the check could not run because an existing permission or approval path
    // refused it, or the evidence does not exist.
    bool refused = false;
    std::string refusalReason;
};

struct ProposedHypothesis
{
    std::string text;
};

struct RoundResult
{
    std::vector<CheckOutcome> outcomes;
    std::vector<ProposedQuestion> followUps;
    std::vector<ProposedHypothesis> newHypotheses;
    // The investigator's opinion. The runtime checks it rather than obeying it.
    bool proposeComplete = false;
    std::string completionRationale;
    std::size_t toolCallsUsed = 0;
    std::size_t tokensUsed = 0;
};

// Runs one round. Supplied by the caller: a fixture in tests, the model and the existing
// tool paths in production.
using RoundRunner = std::function<RoundResult(const RoundRequest&)>;

// Called after each round so the shell can show "Revia is checking..." then "Findings"
// as they happen rather than after the whole investigation.
// Which half of the round this report is. Carried explicitly rather than inferred from
// whether a summary happens to be empty -- a round that finds nothing still has a
// findings half, and guessing from emptiness printed "checking" twice for it.
enum class RoundPhase
{
    Checking,
    Findings
};

struct RoundReport
{
    RoundPhase phase = RoundPhase::Checking;
    std::size_t round = 0;
    std::vector<std::string> questionsChecked;
    std::string checkingSummary;
    std::string findingsSummary;
    std::string evidenceDetail;
    std::size_t newFindings = 0;
    std::size_t newQuestions = 0;
};

using RoundObserver = std::function<void(const RoundReport&)>;

struct InvestigationRunReport
{
    InvestigationOutcome outcome = InvestigationOutcome::Running;
    std::size_t rounds = 0;
    std::size_t toolCallsUsed = 0;
    std::size_t tokensUsed = 0;
    double elapsedMilliseconds = 0.0;
    // Always populated, and honest: says which bound was hit, or why it stopped.
    std::string reason;
    // Rounds that produced neither a finding nor a new question.
    std::size_t noProgressRounds = 0;
    // True when the investigator asked to finish and the runtime's checks refused.
    bool completionRefused = false;
    std::string completionRefusalReason;
};

// Drives the loop.
//
// Nothing here generates rounds in advance. Each round is planned from the state the
// previous round left behind, which is the only way a later question can be chosen using
// an earlier result.
class InvestigationLoop
{
public:
    explicit InvestigationLoop(InvestigationBudget inputBudget = {})
        : budget(inputBudget)
    {
    }

    [[nodiscard]] InvestigationRunReport Run(
        Investigation& investigation,
        const RoundRunner& runner,
        std::stop_token stopToken = {},
        const RoundObserver& observer = {}) const;

    [[nodiscard]] const InvestigationBudget& Budget() const { return budget; }
    void SetBudget(InvestigationBudget value) { budget = value; }

    // The runtime's own completion check, separate from the investigator's opinion.
    //
    // Exposed so it can be tested directly: "the model said it was done" is exactly the
    // claim that needs independent verification.
    [[nodiscard]] static bool MayComplete(
        const Investigation& investigation, std::string& outRefusal);

private:
    InvestigationBudget budget;
};

} // namespace revia::agents
