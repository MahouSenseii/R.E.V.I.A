#include "Agents/investigation.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace revia::agents
{

namespace
{

std::string Trim(const std::string& value)
{
    const std::size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string OneLine(const std::string& value, const std::size_t limit = 160)
{
    std::string flattened;
    flattened.reserve(std::min(value.size(), limit));
    bool lastWasSpace = false;
    for (const char character : value)
    {
        const char mapped =
            (character == '\n' || character == '\r' || character == '\t') ? ' ' : character;
        if (mapped == ' ')
        {
            if (lastWasSpace) continue;
            lastWasSpace = true;
        }
        else
        {
            lastWasSpace = false;
        }
        flattened.push_back(mapped);
        if (flattened.size() >= limit) break;
    }
    return Trim(flattened);
}

} // namespace

std::string ToString(const QuestionStatus status)
{
    switch (status)
    {
        case QuestionStatus::Open: return "Open";
        case QuestionStatus::Checking: return "Checking";
        case QuestionStatus::Supported: return "Supported";
        case QuestionStatus::Refuted: return "Refuted";
        case QuestionStatus::Unresolved: return "Unresolved";
        case QuestionStatus::Blocked: return "Blocked";
    }
    return "Open";
}

bool IsSettled(const QuestionStatus status)
{
    // Unresolved and Blocked are settled for the loop's purposes: it has done what it can
    // and must not keep re-asking. They remain visibly unfinished in the report, which is
    // the honest position -- checked and not established is not the same as answered.
    return status == QuestionStatus::Supported || status == QuestionStatus::Refuted ||
        status == QuestionStatus::Unresolved || status == QuestionStatus::Blocked;
}

std::string ToString(const EvidenceKind kind)
{
    return kind == EvidenceKind::ToolObservation ? "observed" : "interpreted";
}

std::string ToString(const CheckKind kind)
{
    switch (kind)
    {
        case CheckKind::ResolvedConfiguration: return "resolved configuration";
        case CheckKind::SourceCode: return "source";
        case CheckKind::LogsAndMeasurements: return "logs and measurements";
        case CheckKind::Tests: return "tests";
        case CheckKind::FileOrApplicationState: return "file or application state";
        case CheckKind::Research: return "research";
        case CheckKind::Calculation: return "calculation";
        case CheckKind::ModelReasoning: return "reasoning";
    }
    return "reasoning";
}

bool ProducesObservation(const CheckKind kind)
{
    // Reasoning is the one kind that does not. Everything else consulted something
    // outside the model.
    return kind != CheckKind::ModelReasoning;
}

std::string ToString(const Hypothesis::Standing standing)
{
    switch (standing)
    {
        case Hypothesis::Standing::Proposed: return "proposed";
        case Hypothesis::Standing::Supported: return "supported";
        case Hypothesis::Standing::Contradicted: return "contradicted";
        case Hypothesis::Standing::Abandoned: return "abandoned";
    }
    return "proposed";
}

std::string ToString(const InvestigationOutcome outcome)
{
    switch (outcome)
    {
        case InvestigationOutcome::Running: return "running";
        case InvestigationOutcome::Completed: return "completed";
        case InvestigationOutcome::Paused: return "paused";
        case InvestigationOutcome::Blocked: return "blocked";
        case InvestigationOutcome::Cancelled: return "cancelled";
    }
    return "running";
}

std::string InvestigationQuestion::Normalized() const
{
    // Lowercased, stripped of punctuation and repeated spaces. Two questions that differ
    // only in wording are the same question, and asking one twice is the loop spinning.
    std::string normalized;
    normalized.reserve(text.size());
    bool lastWasSpace = true;
    for (const unsigned char character : text)
    {
        if (std::isalnum(character) != 0)
        {
            normalized.push_back(static_cast<char>(std::tolower(character)));
            lastWasSpace = false;
        }
        else if (!lastWasSpace)
        {
            normalized.push_back(' ');
            lastWasSpace = true;
        }
    }
    return Trim(normalized);
}

Investigation::Investigation(
    const std::uint64_t inputTaskId, std::string inputGoal, std::string inputCriteria)
    : taskId(inputTaskId),
      goal(std::move(inputGoal)),
      completionCriteria(std::move(inputCriteria))
{
}

bool Investigation::HasEquivalentQuestion(
    const std::string& normalized, const InvestigationQuestion** outExisting) const
{
    for (const InvestigationQuestion& question : questions)
    {
        if (question.Normalized() == normalized)
        {
            if (outExisting != nullptr) *outExisting = &question;
            return true;
        }
    }
    return false;
}

std::string Investigation::AddQuestion(
    const std::string& text,
    const double materiality,
    const std::size_t round,
    std::vector<std::string> dependsOn,
    const std::string& justification)
{
    const std::string trimmed = Trim(text);
    if (trimmed.empty()) return {};

    InvestigationQuestion candidate;
    candidate.text = trimmed;
    const std::string normalized = candidate.Normalized();
    if (normalized.empty()) return {};

    const InvestigationQuestion* existing = nullptr;
    if (HasEquivalentQuestion(normalized, &existing))
    {
        // Already asked. Reopening is allowed, but only against new evidence -- otherwise
        // the loop would re-ask its way past every bound it has.
        if (justification.empty()) return {};
        if (existing == nullptr || !IsSettled(existing->status)) return {};

        for (InvestigationQuestion& question : questions)
        {
            if (question.Normalized() != normalized) continue;
            question.status = QuestionStatus::Open;
            question.reopenedBecause = Trim(justification);
            question.raisedInRound = round;
            return question.id;
        }
        return {};
    }

    candidate.id = "q" + std::to_string(nextId++);
    candidate.materiality = std::clamp(materiality, 0.0, 1.0);
    candidate.raisedInRound = round;
    candidate.dependsOn = std::move(dependsOn);
    questions.push_back(std::move(candidate));
    return questions.back().id;
}

std::string Investigation::AddHypothesis(const std::string& text)
{
    const std::string trimmed = Trim(text);
    if (trimmed.empty()) return {};
    for (const Hypothesis& hypothesis : hypotheses)
    {
        if (hypothesis.text == trimmed) return hypothesis.id;
    }
    Hypothesis hypothesis;
    hypothesis.id = "h" + std::to_string(nextId++);
    hypothesis.text = trimmed;
    hypotheses.push_back(std::move(hypothesis));
    return hypotheses.back().id;
}

std::string Investigation::RecordCheck(InvestigationCheck check)
{
    if (check.id.empty()) check.id = "c" + std::to_string(nextId++);
    checks.push_back(std::move(check));
    return checks.back().id;
}

std::string Investigation::RecordFinding(Finding finding)
{
    // A finding with no check behind it is an assertion wearing a finding's clothes.
    if (finding.checkId.empty()) return {};
    if (finding.id.empty()) finding.id = "f" + std::to_string(nextId++);

    const std::string findingId = finding.id;
    const std::string questionId = finding.questionId;

    for (const std::string& hypothesisId : finding.supportsHypotheses)
    {
        for (Hypothesis& hypothesis : hypotheses)
        {
            if (hypothesis.id != hypothesisId) continue;
            hypothesis.supportedBy.push_back(findingId);
            // Support does not lift a contradiction. A hypothesis with evidence on both
            // sides stays contradicted until something resolves it, because the awkward
            // half is the half worth keeping.
            if (hypothesis.standing == Hypothesis::Standing::Proposed)
            {
                hypothesis.standing = Hypothesis::Standing::Supported;
            }
        }
    }
    for (const std::string& hypothesisId : finding.contradictsHypotheses)
    {
        for (Hypothesis& hypothesis : hypotheses)
        {
            if (hypothesis.id != hypothesisId) continue;
            hypothesis.contradictedBy.push_back(findingId);
            hypothesis.standing = Hypothesis::Standing::Contradicted;
        }
    }

    findings.push_back(std::move(finding));
    for (InvestigationQuestion& question : questions)
    {
        if (question.id == questionId) question.findingIds.push_back(findingId);
    }
    return findingId;
}

void Investigation::SetQuestionStatus(
    const std::string& questionId, const QuestionStatus status)
{
    for (InvestigationQuestion& question : questions)
    {
        if (question.id == questionId) question.status = status;
    }
}

void Investigation::MarkChecked(const std::string& questionId, const std::size_t round)
{
    for (InvestigationQuestion& question : questions)
    {
        if (question.id == questionId) question.checkedInRound = round;
    }
}

void Investigation::SetHypothesisStanding(
    const std::string& id, const Hypothesis::Standing standing)
{
    for (Hypothesis& hypothesis : hypotheses)
    {
        if (hypothesis.id == id) hypothesis.standing = standing;
    }
}

std::vector<InvestigationQuestion> Investigation::Selectable() const
{
    std::vector<InvestigationQuestion> ready;
    for (const InvestigationQuestion& question : questions)
    {
        if (question.status != QuestionStatus::Open) continue;

        // A dependent check waits for its inputs. Running it early would produce an
        // answer derived from a question nobody has settled yet.
        bool dependenciesSettled = true;
        for (const std::string& dependency : question.dependsOn)
        {
            const InvestigationQuestion* input = FindQuestion(dependency);
            if (input == nullptr || !IsSettled(input->status))
            {
                dependenciesSettled = false;
                break;
            }
        }
        if (dependenciesSettled) ready.push_back(question);
    }
    return ready;
}

std::vector<InvestigationQuestion> Investigation::SelectForRound(
    const std::size_t limit) const
{
    std::vector<InvestigationQuestion> ready = Selectable();
    // Most material first: the question that could change the answer is worth more than
    // the question that is merely easy. Stable on round then id so the order is
    // reproducible, which matters for a deterministic test.
    std::stable_sort(ready.begin(), ready.end(),
        [](const InvestigationQuestion& a, const InvestigationQuestion& b)
        {
            if (a.materiality != b.materiality) return a.materiality > b.materiality;
            return a.raisedInRound < b.raisedInRound;
        });
    if (ready.size() > limit) ready.resize(limit);
    return ready;
}

const InvestigationQuestion* Investigation::FindQuestion(const std::string& id) const
{
    for (const InvestigationQuestion& question : questions)
    {
        if (question.id == id) return &question;
    }
    return nullptr;
}

std::size_t Investigation::PendingCount() const
{
    std::size_t pending = 0;
    for (const InvestigationQuestion& question : questions)
    {
        if (!IsSettled(question.status)) ++pending;
    }
    return pending;
}

std::size_t Investigation::ObservationCount() const
{
    std::size_t observations = 0;
    for (const Finding& finding : findings)
    {
        if (finding.kind == EvidenceKind::ToolObservation) ++observations;
    }
    return observations;
}

std::vector<std::string> Investigation::MaterialOpenQuestions(const double threshold) const
{
    std::vector<std::string> open;
    for (const InvestigationQuestion& question : questions)
    {
        if (IsSettled(question.status)) continue;
        if (question.materiality >= threshold) open.push_back(question.text);
    }
    return open;
}

std::vector<std::string> Investigation::UnresolvedContradictions() const
{
    std::vector<std::string> unresolved;
    for (const Hypothesis& hypothesis : hypotheses)
    {
        if (hypothesis.standing == Hypothesis::Standing::Contradicted)
        {
            unresolved.push_back(hypothesis.text);
        }
    }
    return unresolved;
}

std::vector<std::string> Investigation::Blockers() const
{
    std::vector<std::string> blockers;
    for (const InvestigationQuestion& question : questions)
    {
        if (question.status == QuestionStatus::Blocked) blockers.push_back(question.text);
    }
    for (const InvestigationCheck& check : checks)
    {
        if (!check.refusal.empty()) blockers.push_back(check.refusal);
    }
    return blockers;
}

std::string Investigation::CheckingSummary(const std::size_t round) const
{
    std::ostringstream text;
    bool first = true;
    for (const InvestigationQuestion& question : questions)
    {
        if (question.checkedInRound != round) continue;
        if (!first) text << "\n";
        text << "- " << OneLine(question.text, 120);
        first = false;
    }
    return text.str();
}

std::string Investigation::FindingsSummary(const std::size_t round) const
{
    std::ostringstream text;
    bool first = true;
    for (const Finding& finding : findings)
    {
        if (finding.round != round) continue;
        if (!first) text << "\n";
        // The evidence kind is shown, not hidden. "Interpreted" and "observed" are
        // different claims and the reader is entitled to know which one this is.
        text << "- [" << ToString(finding.kind) << "] " << OneLine(finding.observed, 120);
        first = false;
    }
    for (const InvestigationQuestion& question : questions)
    {
        if (question.checkedInRound != round) continue;
        if (question.status != QuestionStatus::Blocked &&
            question.status != QuestionStatus::Unresolved)
        {
            continue;
        }
        if (!first) text << "\n";
        text << "- [" << ToString(question.status) << "] " << OneLine(question.text, 100);
        first = false;
    }
    return text.str();
}

std::string Investigation::EvidenceDetail(const std::size_t round) const
{
    std::ostringstream text;
    for (const Finding& finding : findings)
    {
        if (finding.round != round) continue;
        const InvestigationQuestion* question = FindQuestion(finding.questionId);
        text << "Question: " << (question != nullptr ? question->text : finding.questionId)
             << "\n";
        for (const InvestigationCheck& check : checks)
        {
            if (check.id != finding.checkId) continue;
            text << "Check: " << check.description << " (" << ToString(check.kind) << ")\n";
        }
        text << "Observed: " << finding.observed << "\n";
        if (!finding.limitations.empty())
        {
            text << "Limits: " << finding.limitations << "\n";
        }
        if (!finding.contradictsHypotheses.empty())
        {
            text << "Contradicts: ";
            for (const std::string& id : finding.contradictsHypotheses)
            {
                for (const Hypothesis& hypothesis : hypotheses)
                {
                    if (hypothesis.id == id) text << hypothesis.text << " ";
                }
            }
            text << "\n";
        }
        text << "\n";
    }
    return text.str();
}

std::string Investigation::PromptBlock() const
{
    if (findings.empty() && questions.empty()) return {};

    std::ostringstream text;
    text << "What I checked before answering, and what I found:\n";
    for (const InvestigationQuestion& question : questions)
    {
        text << "- (" << ToString(question.status) << ") " << question.text << "\n";
        for (const std::string& findingId : question.findingIds)
        {
            for (const Finding& finding : findings)
            {
                if (finding.id != findingId) continue;
                text << "    [" << ToString(finding.kind) << "] " << finding.observed;
                if (!finding.limitations.empty())
                {
                    text << " (limits: " << finding.limitations << ")";
                }
                text << "\n";
            }
        }
    }
    const std::vector<std::string> contradictions = UnresolvedContradictions();
    if (!contradictions.empty())
    {
        // Carried into the answer rather than quietly dropped. An unresolved
        // contradiction the reader is not told about is the worst outcome available.
        text << "Contradicted and not resolved:\n";
        for (const std::string& contradiction : contradictions)
        {
            text << "- " << contradiction << "\n";
        }
    }
    const std::vector<std::string> blockers = Blockers();
    if (!blockers.empty())
    {
        text << "Could not check:\n";
        for (const std::string& blocker : blockers) text << "- " << blocker << "\n";
    }
    text << "Answer from what is actually supported above. Say plainly where the evidence "
            "is missing rather than filling the gap.";
    return text.str();
}

// ---------------------------------------------------------------- the loop

bool InvestigationLoop::MayComplete(
    const Investigation& investigation, std::string& outRefusal)
{
    // The investigator's opinion is an input, not a decision. These are the runtime's own
    // checks, and they are the reason "the model said it was finished" is not sufficient.
    const std::vector<std::string> openMaterial = investigation.MaterialOpenQuestions(0.6);
    if (!openMaterial.empty())
    {
        outRefusal = "still open and material: " + openMaterial.front();
        return false;
    }
    if (investigation.Findings().empty())
    {
        outRefusal = "nothing was actually established";
        return false;
    }
    // Recording a finding is not the same as settling a question. An investigation whose
    // every question came back Unresolved or Blocked has answered nothing, however much
    // it wrote down on the way -- and calling that Completed would be the exact failure
    // this whole design exists to prevent.
    bool anythingSettled = false;
    for (const InvestigationQuestion& question : investigation.Questions())
    {
        if (question.status == QuestionStatus::Supported ||
            question.status == QuestionStatus::Refuted)
        {
            anythingSettled = true;
            break;
        }
    }
    if (!anythingSettled)
    {
        outRefusal = "no question was settled either way";
        return false;
    }
    const std::vector<std::string> contradictions =
        investigation.UnresolvedContradictions();
    if (!contradictions.empty())
    {
        // Allowed to finish *only* because PromptBlock carries the contradiction into the
        // answer. Silently completing over it would not be.
        outRefusal.clear();
        return true;
    }
    outRefusal.clear();
    return true;
}

InvestigationRunReport InvestigationLoop::Run(
    Investigation& investigation,
    const RoundRunner& runner,
    const std::stop_token stopToken,
    const RoundObserver& observer) const
{
    InvestigationRunReport report;
    const auto started = std::chrono::steady_clock::now();

    const auto elapsed = [&started]()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
    };
    const auto finish = [&](const InvestigationOutcome outcome, std::string reason)
    {
        report.outcome = outcome;
        report.reason = std::move(reason);
        report.elapsedMilliseconds = static_cast<double>(elapsed().count());
        return report;
    };

    if (!runner) return finish(InvestigationOutcome::Blocked, "No investigator was supplied.");

    for (std::size_t round = 1; round <= budget.maximumRounds; ++round)
    {
        if (stopToken.stop_requested())
        {
            return finish(InvestigationOutcome::Cancelled,
                "The investigation was cancelled before round " + std::to_string(round) + ".");
        }
        if (elapsed() >= budget.wallClock)
        {
            return finish(InvestigationOutcome::Paused,
                "The time budget was reached after " + std::to_string(report.rounds) +
                " round(s). This is a pause, not an answer.");
        }
        if (report.toolCallsUsed >= budget.maximumToolCalls)
        {
            return finish(InvestigationOutcome::Paused,
                "The tool-call budget was reached. This is a pause, not an answer.");
        }
        if (report.tokensUsed >= budget.maximumTokens)
        {
            return finish(InvestigationOutcome::Paused,
                "The token budget was reached. This is a pause, not an answer.");
        }

        // Planned here, from the state the previous round left behind. Nothing about this
        // round existed before the last one finished.
        const std::vector<InvestigationQuestion> selected =
            investigation.SelectForRound(budget.maximumQuestionsPerRound);
        if (selected.empty())
        {
            if (investigation.PendingCount() > 0)
            {
                // Everything left is waiting on a dependency that never settled.
                return finish(InvestigationOutcome::Blocked,
                    "Every remaining question depends on something that could not be "
                    "settled.");
            }
            std::string refusal;
            if (InvestigationLoop::MayComplete(investigation, refusal))
            {
                return finish(InvestigationOutcome::Completed,
                    "Every question was settled.");
            }
            // Nothing left to check, and the runtime's completion checks still refuse.
            // Blocked rather than Completed: the loop ran out of things it could do, not
            // out of things that needed doing.
            return finish(InvestigationOutcome::Blocked,
                "Nothing further could be checked: " + refusal);
        }

        RoundRequest request;
        request.taskId = investigation.TaskId();
        request.round = round;
        request.goal = investigation.Goal();
        request.questions = selected;
        request.priorFindings = investigation.Findings();
        request.hypotheses = investigation.Hypotheses();
        request.toolCallsRemaining = budget.maximumToolCalls > report.toolCallsUsed
            ? budget.maximumToolCalls - report.toolCallsUsed : 0;
        request.timeRemaining = budget.wallClock > elapsed()
            ? budget.wallClock - elapsed() : std::chrono::milliseconds{0};

        for (const InvestigationQuestion& question : selected)
        {
            investigation.SetQuestionStatus(question.id, QuestionStatus::Checking);
        }
        // Recorded before the round runs, so the transcript can show what is being
        // checked while it is still being checked.
        for (const InvestigationQuestion& question : selected)
        {
            investigation.MarkChecked(question.id, round);
        }

        RoundReport roundReport;
        roundReport.phase = RoundPhase::Checking;
        roundReport.round = round;
        for (const InvestigationQuestion& question : selected)
        {
            roundReport.questionsChecked.push_back(question.text);
        }
        roundReport.checkingSummary = investigation.CheckingSummary(round);
        if (observer) observer(roundReport);

        const RoundResult result = runner(request);
        ++report.rounds;
        report.toolCallsUsed += result.toolCallsUsed;
        report.tokensUsed += result.tokensUsed;

        if (stopToken.stop_requested())
        {
            // Late results from a cancelled investigation are discarded rather than
            // recorded. A finding that arrives after the user moved on must not appear
            // under their next question.
            return finish(InvestigationOutcome::Cancelled,
                "Cancelled during round " + std::to_string(round) +
                "; results from that round were discarded.");
        }

        for (const ProposedHypothesis& proposed : result.newHypotheses)
        {
            investigation.AddHypothesis(proposed.text);
        }

        std::size_t newFindings = 0;
        for (const CheckOutcome& outcome : result.outcomes)
        {
            const InvestigationQuestion* question =
                investigation.FindQuestion(outcome.questionId);
            if (question == nullptr) continue;

            InvestigationCheck check;
            check.questionId = outcome.questionId;
            check.description = outcome.checkDescription;
            check.kind = outcome.checkKind;
            check.attempted = true;
            check.completed = !outcome.refused;
            check.refusal = outcome.refused ? outcome.refusalReason : std::string{};
            const std::string checkId = investigation.RecordCheck(std::move(check));

            if (outcome.refused)
            {
                // Refused or unavailable is Blocked. It is never an excuse to invent a
                // result, and it is never Supported.
                investigation.SetQuestionStatus(outcome.questionId, QuestionStatus::Blocked);
                continue;
            }

            Finding finding;
            finding.questionId = outcome.questionId;
            finding.checkId = checkId;
            // The kind follows the check that produced it, not what the caller claims.
            // A model reasoning about something cannot report a tool observation.
            finding.kind = ProducesObservation(outcome.checkKind)
                ? EvidenceKind::ToolObservation : EvidenceKind::ModelInterpretation;
            finding.observed = outcome.observed;
            finding.limitations = outcome.limitations;
            finding.supportsHypotheses = outcome.supportsHypotheses;
            finding.contradictsHypotheses = outcome.contradictsHypotheses;
            finding.round = round;
            if (!finding.observed.empty() &&
                !investigation.RecordFinding(std::move(finding)).empty())
            {
                ++newFindings;
            }
            investigation.SetQuestionStatus(outcome.questionId, outcome.status);
        }

        std::size_t newQuestions = 0;
        for (const ProposedQuestion& proposed : result.followUps)
        {
            if (investigation.PendingCount() >= budget.maximumPendingQuestions) break;
            const std::string id = investigation.AddQuestion(
                proposed.text, proposed.materiality, round, proposed.dependsOn,
                proposed.justification);
            if (!id.empty()) ++newQuestions;
        }

        roundReport.phase = RoundPhase::Findings;
        roundReport.findingsSummary = investigation.FindingsSummary(round);
        roundReport.evidenceDetail = investigation.EvidenceDetail(round);
        roundReport.newFindings = newFindings;
        roundReport.newQuestions = newQuestions;
        if (observer) observer(roundReport);

        // A round that produced no evidence and no new direction has not moved. Two of
        // those in a row is a loop, not an investigation.
        if (newFindings == 0 && newQuestions == 0)
        {
            ++report.noProgressRounds;
            if (report.noProgressRounds >= budget.maximumNoProgressRounds)
            {
                return finish(InvestigationOutcome::Paused,
                    "Stopped after " + std::to_string(report.noProgressRounds) +
                    " rounds that produced no new evidence and no new question.");
            }
        }
        else
        {
            report.noProgressRounds = 0;
        }

        if (result.proposeComplete)
        {
            std::string refusal;
            if (MayComplete(investigation, refusal))
            {
                return finish(InvestigationOutcome::Completed,
                    result.completionRationale.empty()
                        ? "The investigation reached a supported answer."
                        : result.completionRationale);
            }
            // The investigator wanted to stop and the runtime would not let it. Recorded,
            // because "it said it was done" is exactly the claim worth auditing.
            report.completionRefused = true;
            report.completionRefusalReason = refusal;
        }
    }

    return finish(InvestigationOutcome::Paused,
        "The round budget was reached after " + std::to_string(report.rounds) +
        " round(s). This is a pause, not an answer.");
}

} // namespace revia::agents
