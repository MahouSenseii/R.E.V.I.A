#include "testSupport.h"

#include "Agents/investigation.h"
#include "Agents/investigationAgent.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace revia::agents;
using revia::tests::Check;

// A fixture investigator.
//
// These prove the *loop* behaves correctly given results: that it selects multiple
// questions, carries evidence forward, refuses to finish early, pauses rather than
// claiming success, and discards late work. They prove nothing at all about whether a
// language model reliably conducts an investigation -- that is a separate question,
// answered separately, and a scripted fixture must never be presented as evidence for it.

CheckOutcome Observed(
    const std::string& questionId,
    const QuestionStatus status,
    const std::string& observed,
    const CheckKind kind = CheckKind::SourceCode)
{
    CheckOutcome outcome;
    outcome.questionId = questionId;
    outcome.status = status;
    outcome.checkKind = kind;
    outcome.checkDescription = "read " + questionId;
    outcome.observed = observed;
    return outcome;
}

Investigation Start(const std::string& goal = "Why is the build slow?")
{
    return Investigation(1, goal, "A cause supported by a measurement.");
}

// 1. More than one question in a round.
void TestMultipleQuestionsInOneRound()
{
    Investigation investigation = Start();
    investigation.AddQuestion("Is the compiler cache enabled?", 0.8, 0);
    investigation.AddQuestion("How many translation units are rebuilt?", 0.7, 0);
    investigation.AddQuestion("Is the link step the bottleneck?", 0.6, 0);

    std::size_t questionsInFirstRound = 0;
    InvestigationLoop loop;
    const RoundRunner runner = [&](const RoundRequest& request)
    {
        if (request.round == 1) questionsInFirstRound = request.questions.size();
        RoundResult result;
        for (const InvestigationQuestion& question : request.questions)
        {
            result.outcomes.push_back(
                Observed(question.id, QuestionStatus::Supported, "checked " + question.id));
        }
        result.proposeComplete = true;
        return result;
    };

    const InvestigationRunReport report = loop.Run(investigation, runner);
    Check(questionsInFirstRound == 3,
        "A round carried " + std::to_string(questionsInFirstRound) +
        " questions; the loop must be able to check several at once.");
    Check(report.outcome == InvestigationOutcome::Completed,
        "A fully settled investigation did not complete.");
}

// 2. A finding causes a genuinely new follow-up question.
void TestFindingProducesANewFollowUp()
{
    Investigation investigation = Start();
    investigation.AddQuestion("Is the compiler cache enabled?", 0.9, 0);

    bool followUpWasDerivedFromEvidence = false;
    InvestigationLoop loop;
    const RoundRunner runner = [&](const RoundRequest& request)
    {
        RoundResult result;
        if (request.round == 1)
        {
            result.outcomes.push_back(Observed(
                request.questions.front().id, QuestionStatus::Refuted,
                "ccache is disabled in the release preset"));
            // The follow-up exists *because* of what was observed.
            ProposedQuestion followUp;
            followUp.text = "Why is ccache disabled in the release preset?";
            followUp.materiality = 0.85;
            result.followUps.push_back(followUp);
            return result;
        }

        // The decisive check: the second round can only ask this because it was handed
        // the first round's actual observation. A loop that generated both rounds up
        // front would not have this text available.
        for (const Finding& finding : request.priorFindings)
        {
            if (finding.observed.find("ccache is disabled") != std::string::npos)
            {
                followUpWasDerivedFromEvidence = true;
            }
        }
        for (const InvestigationQuestion& question : request.questions)
        {
            result.outcomes.push_back(Observed(
                question.id, QuestionStatus::Supported,
                "the preset sets CMAKE_C_COMPILER_LAUNCHER only for Debug"));
        }
        result.proposeComplete = true;
        return result;
    };

    const InvestigationRunReport report = loop.Run(investigation, runner);
    Check(report.rounds >= 2, "The follow-up never got its own round.");
    Check(followUpWasDerivedFromEvidence,
        "The second round did not receive the first round's evidence, so a later question "
        "could not have been chosen using an earlier result.");

    bool hasFollowUp = false;
    for (const InvestigationQuestion& question : investigation.Questions())
    {
        if (question.raisedInRound == 1 && question.text.find("Why is ccache") == 0)
        {
            hasFollowUp = true;
        }
    }
    Check(hasFollowUp, "The follow-up question was not recorded against the round that "
        "raised it.");
}

// 3. A three-round investigation reaches a supported answer.
void TestThreeRoundInvestigationReachesSupportedAnswer()
{
    Investigation investigation = Start();
    investigation.AddQuestion("Is the compiler cache enabled?", 0.9, 0);

    InvestigationLoop loop;
    const RoundRunner runner = [&](const RoundRequest& request)
    {
        RoundResult result;
        const std::string id = request.questions.front().id;
        if (request.round == 1)
        {
            result.outcomes.push_back(Observed(
                id, QuestionStatus::Refuted, "ccache is disabled for Release"));
            ProposedQuestion next;
            next.text = "Which preset disables it?";
            next.materiality = 0.8;
            result.followUps.push_back(next);
        }
        else if (request.round == 2)
        {
            result.outcomes.push_back(Observed(
                id, QuestionStatus::Supported, "the release preset omits the launcher"));
            ProposedQuestion next;
            next.text = "Does enabling it restore the build time?";
            next.materiality = 0.95;
            result.followUps.push_back(next);
        }
        else
        {
            result.outcomes.push_back(Observed(
                id, QuestionStatus::Supported,
                "a rebuild with the launcher set took 41s against 186s",
                CheckKind::LogsAndMeasurements));
            result.proposeComplete = true;
            result.completionRationale = "The cause is supported by a measurement.";
        }
        return result;
    };

    const InvestigationRunReport report = loop.Run(investigation, runner);
    Check(report.rounds == 3, "Expected three rounds, ran " +
        std::to_string(report.rounds) + ".");
    Check(report.outcome == InvestigationOutcome::Completed,
        "The investigation did not complete: " + report.reason);
    Check(investigation.ObservationCount() >= 1,
        "The supported answer rested on no tool observation at all.");
    Check(investigation.PromptBlock().find("41s") != std::string::npos,
        "The measurement that settled it did not reach the answer's prompt block.");
}

// 4. Contradictory evidence changes the hypothesis.
void TestContradictionChangesDirection()
{
    Investigation investigation = Start();
    const std::string hypothesisId =
        investigation.AddHypothesis("The link step is the bottleneck.");
    investigation.AddQuestion("How long does linking take?", 0.9, 0);

    InvestigationLoop loop;
    const RoundRunner runner = [&](const RoundRequest& request)
    {
        RoundResult result;
        const std::string id = request.questions.front().id;
        if (request.round == 1)
        {
            CheckOutcome outcome = Observed(
                id, QuestionStatus::Refuted, "linking accounts for 4s of a 186s build",
                CheckKind::LogsAndMeasurements);
            outcome.contradictsHypotheses = {hypothesisId};
            result.outcomes.push_back(outcome);

            // Direction changes rather than the original guess being retried.
            ProposedHypothesis replacement;
            replacement.text = "Compilation, not linking, dominates the build.";
            result.newHypotheses.push_back(replacement);
            ProposedQuestion next;
            next.text = "Which translation units take longest to compile?";
            next.materiality = 0.9;
            result.followUps.push_back(next);
            return result;
        }
        result.outcomes.push_back(Observed(
            id, QuestionStatus::Supported, "six units account for 70% of compile time",
            CheckKind::LogsAndMeasurements));
        result.proposeComplete = true;
        return result;
    };

    const InvestigationRunReport report = loop.Run(investigation, runner);
    Check(report.rounds == 2, "The loop did not change direction in a second round.");

    const Hypothesis* original = nullptr;
    for (const Hypothesis& hypothesis : investigation.Hypotheses())
    {
        if (hypothesis.id == hypothesisId) original = &hypothesis;
    }
    Check(original != nullptr, "The original hypothesis vanished.");
    Check(original->standing == Hypothesis::Standing::Contradicted,
        "A contradicted hypothesis was not marked as such.");
    Check(!original->contradictedBy.empty(),
        "The contradiction was recorded without the evidence that produced it.");
    Check(investigation.Hypotheses().size() == 2,
        "The loop did not adopt a replacement hypothesis.");

    // The contradiction is carried into the answer rather than quietly dropped.
    Check(investigation.PromptBlock().find("Contradicted and not resolved") !=
        std::string::npos,
        "An unresolved contradiction did not reach the answer.");
}

// 5. Repeated questions without new evidence trigger no-progress handling.
void TestRepeatedQuestionsTriggerNoProgress()
{
    Investigation investigation = Start();
    investigation.AddQuestion("Is the compiler cache enabled?", 0.9, 0);

    InvestigationBudget budget;
    budget.maximumNoProgressRounds = 2;
    budget.maximumRounds = 8;
    InvestigationLoop loop(budget);

    std::size_t roundsRun = 0;
    const RoundRunner runner = [&](const RoundRequest& request)
    {
        ++roundsRun;
        RoundResult result;
        if (request.round == 1)
        {
            result.outcomes.push_back(Observed(
                request.questions.front().id, QuestionStatus::Unresolved,
                "the preset could not be read"));
        }
        // Every later round re-asks the same question in slightly different words and
        // brings nothing new. This is the shape of a loop that has stopped working.
        ProposedQuestion repeat;
        repeat.text = "Is the Compiler Cache enabled?";
        repeat.materiality = 0.9;
        result.followUps.push_back(repeat);
        return result;
    };

    const InvestigationRunReport report = loop.Run(investigation, runner);
    Check(report.outcome == InvestigationOutcome::Paused ||
        report.outcome == InvestigationOutcome::Blocked,
        "A loop making no progress reported " + ToString(report.outcome) + ".");
    Check(report.rounds < 8, "The loop ran to its round ceiling instead of noticing that "
        "it had stopped progressing.");
    Check(investigation.Questions().size() == 1,
        "A re-worded duplicate was admitted as a new question.");
}

// 6. Missing evidence produces Unresolved or Blocked.
void TestMissingEvidenceIsBlockedNotInvented()
{
    Investigation investigation = Start();
    investigation.AddQuestion("What did the CI log say?", 0.9, 0);

    InvestigationLoop loop;
    const RoundRunner runner = [&](const RoundRequest& request)
    {
        RoundResult result;
        CheckOutcome outcome;
        outcome.questionId = request.questions.front().id;
        outcome.checkKind = CheckKind::LogsAndMeasurements;
        outcome.checkDescription = "read the CI log";
        outcome.refused = true;
        outcome.refusalReason = "no CI log exists for this branch";
        result.outcomes.push_back(outcome);
        return result;
    };

    const InvestigationRunReport report = loop.Run(investigation, runner);
    const InvestigationQuestion& question = investigation.Questions().front();
    Check(question.status == QuestionStatus::Blocked,
        "Unavailable evidence produced " + ToString(question.status) +
        " instead of Blocked.");
    Check(investigation.Findings().empty(),
        "A refused check still produced a finding. Missing evidence must never be filled "
        "in with a result.");
    Check(report.outcome != InvestigationOutcome::Completed,
        "An investigation with no evidence reported completion.");
    Check(!investigation.Blockers().empty(), "The blocker was not recorded.");
    Check(investigation.PromptBlock().find("Could not check") != std::string::npos,
        "The answer was not told what could not be checked.");
}

// 7. Budget exhaustion pauses without claiming success.
void TestBudgetExhaustionPausesWithoutSuccess()
{
    Investigation investigation = Start();
    investigation.AddQuestion("Question one", 0.9, 0);

    InvestigationBudget budget;
    budget.maximumRounds = 2;
    InvestigationLoop loop(budget);

    const RoundRunner runner = [&](const RoundRequest& request)
    {
        RoundResult result;
        result.outcomes.push_back(Observed(
            request.questions.front().id, QuestionStatus::Supported, "partial evidence"));
        // Always raises another question, so the loop can never run out of work.
        ProposedQuestion next;
        next.text = "Follow-up for round " + std::to_string(request.round);
        next.materiality = 0.9;
        result.followUps.push_back(next);
        return result;
    };

    const InvestigationRunReport report = loop.Run(investigation, runner);
    Check(report.outcome == InvestigationOutcome::Paused,
        "A budget ceiling produced " + ToString(report.outcome) + " rather than Paused.");
    Check(report.reason.find("not an answer") != std::string::npos,
        "The pause did not say plainly that it is not an answer: " + report.reason);
    Check(report.rounds == 2, "The round ceiling was not enforced.");
    Check(investigation.PendingCount() > 0,
        "A paused investigation left nothing outstanding, which would make it finished.");

    // The tool-call ceiling behaves the same way.
    Investigation second = Start();
    second.AddQuestion("Question one", 0.9, 0);
    InvestigationBudget tight;
    tight.maximumToolCalls = 1;
    tight.maximumRounds = 6;
    InvestigationLoop tightLoop(tight);
    const RoundRunner expensive = [&](const RoundRequest& request)
    {
        RoundResult result;
        result.toolCallsUsed = 2;
        result.outcomes.push_back(Observed(
            request.questions.front().id, QuestionStatus::Supported, "something"));
        ProposedQuestion next;
        next.text = "Another for round " + std::to_string(request.round);
        next.materiality = 0.9;
        result.followUps.push_back(next);
        return result;
    };
    const InvestigationRunReport tightReport = tightLoop.Run(second, expensive);
    Check(tightReport.outcome == InvestigationOutcome::Paused,
        "Exhausting the tool-call budget did not pause.");
    Check(tightReport.reason.find("tool-call") != std::string::npos,
        "The pause did not name the budget that ran out: " + tightReport.reason);
}

// 8. Cancellation rejects late results.
void TestCancellationRejectsLateResults()
{
    Investigation investigation = Start();
    investigation.AddQuestion("Is the compiler cache enabled?", 0.9, 0);

    std::stop_source source;
    InvestigationLoop loop;
    const RoundRunner runner = [&](const RoundRequest& request)
    {
        RoundResult result;
        // The check completes, but the user cancelled while it was running. Its result is
        // late, and a late result from a cancelled task must not reach the transcript.
        source.request_stop();
        result.outcomes.push_back(Observed(
            request.questions.front().id, QuestionStatus::Supported,
            "this observation arrived after the user moved on"));
        return result;
    };

    const InvestigationRunReport report = loop.Run(investigation, runner, source.get_token());
    Check(report.outcome == InvestigationOutcome::Cancelled,
        "A cancelled investigation reported " + ToString(report.outcome) + ".");
    Check(investigation.Findings().empty(),
        "A result that arrived after cancellation was recorded anyway.");
    Check(investigation.PromptBlock().find("after the user moved on") == std::string::npos,
        "A stale finding could still appear under a later question.");

    // Cancelled before the first round runs at all.
    Investigation second = Start();
    second.AddQuestion("Anything", 0.9, 0);
    std::stop_source preCancelled;
    preCancelled.request_stop();
    bool ranAnyway = false;
    const RoundRunner shouldNotRun = [&](const RoundRequest&)
    {
        ranAnyway = true;
        return RoundResult{};
    };
    const InvestigationRunReport early =
        loop.Run(second, shouldNotRun, preCancelled.get_token());
    Check(!ranAnyway, "A round ran despite the investigation already being cancelled.");
    Check(early.outcome == InvestigationOutcome::Cancelled, "Pre-cancellation was ignored.");
}

// 9. Hiding work summaries does not change investigation behaviour.
void TestHidingSummariesDoesNotChangeBehaviour()
{
    const auto run = [](const bool observeRounds, InvestigationRunReport& outReport)
    {
        Investigation investigation = Start();
        investigation.AddQuestion("Is the compiler cache enabled?", 0.9, 0);
        InvestigationLoop loop;
        const RoundRunner runner = [&](const RoundRequest& request)
        {
            RoundResult result;
            const std::string id = request.questions.front().id;
            if (request.round == 1)
            {
                result.outcomes.push_back(Observed(
                    id, QuestionStatus::Refuted, "ccache is disabled"));
                ProposedQuestion next;
                next.text = "Which preset disables it?";
                next.materiality = 0.8;
                result.followUps.push_back(next);
                return result;
            }
            result.outcomes.push_back(Observed(
                id, QuestionStatus::Supported, "the release preset omits the launcher"));
            result.proposeComplete = true;
            return result;
        };

        std::size_t observed = 0;
        const RoundObserver observer = [&](const RoundReport&) { ++observed; };
        outReport = observeRounds
            ? loop.Run(investigation, runner, {}, observer)
            : loop.Run(investigation, runner);
        return investigation.PromptBlock();
    };

    InvestigationRunReport shown;
    InvestigationRunReport hidden;
    const std::string shownBlock = run(true, shown);
    const std::string hiddenBlock = run(false, hidden);

    // Display is a separate concern from verification. Turning the panel off must change
    // what the user sees and nothing else.
    Check(shown.outcome == hidden.outcome, "Hiding the panel changed the outcome.");
    Check(shown.rounds == hidden.rounds, "Hiding the panel changed how many rounds ran.");
    Check(shownBlock == hiddenBlock,
        "Hiding the panel changed what the investigation established.");
    Check(!hiddenBlock.empty(),
        "With the panel hidden the investigation stopped producing anything for the "
        "answer, which would make hiding it a way of disabling verification.");
}

// 10. A simple task finishes without unnecessary rounds.
void TestSimpleTaskFinishesInOneRound()
{
    Investigation investigation = Start("Does the config file exist?");
    investigation.AddQuestion("Does the config file exist?", 0.9, 0);

    InvestigationLoop loop;
    const RoundRunner runner = [&](const RoundRequest& request)
    {
        RoundResult result;
        result.outcomes.push_back(Observed(
            request.questions.front().id, QuestionStatus::Supported,
            "the file exists and parses", CheckKind::FileOrApplicationState));
        result.proposeComplete = true;
        result.completionRationale = "One check settled the whole question.";
        return result;
    };

    const InvestigationRunReport report = loop.Run(investigation, runner);
    Check(report.rounds == 1,
        "A one-question task took " + std::to_string(report.rounds) +
        " rounds. Rounds must not be spent to reach a minimum count.");
    Check(report.outcome == InvestigationOutcome::Completed, "A settled question did not "
        "complete: " + report.reason);
}

// The runtime's completion check is independent of the investigator's opinion.
void TestRuntimeRefusesPrematureCompletion()
{
    Investigation investigation = Start();
    investigation.AddQuestion("Cheap question", 0.2, 0);
    investigation.AddQuestion("The question that decides it", 0.95, 0);

    InvestigationBudget budget;
    budget.maximumQuestionsPerRound = 1;
    budget.maximumRounds = 3;
    InvestigationLoop loop(budget);

    std::size_t refusedRounds = 0;
    const RoundRunner runner = [&](const RoundRequest& request)
    {
        RoundResult result;
        result.outcomes.push_back(Observed(
            request.questions.front().id, QuestionStatus::Supported, "settled it"));
        // Claims to be finished every round. The runtime must not simply believe it.
        result.proposeComplete = true;
        return result;
    };

    const InvestigationRunReport report = loop.Run(investigation, runner, {},
        [&](const RoundReport&) { ++refusedRounds; });

    // The most material question is selected first, so after round 1 the low-materiality
    // one is still open -- which is not enough to refuse. The check that matters is that
    // a high-materiality open question *does* refuse, so run it the other way round.
    Investigation strict = Start();
    strict.AddQuestion("Low stakes", 0.2, 0);
    const std::string decisive = strict.AddQuestion("Decides the answer", 0.95, 0);
    strict.SetQuestionStatus(decisive, QuestionStatus::Open);

    std::string refusal;
    Check(!InvestigationLoop::MayComplete(strict, refusal),
        "The runtime allowed completion with a decisive question still open.");
    Check(refusal.find("Decides the answer") != std::string::npos,
        "The refusal did not name the open question: " + refusal);

    // And with nothing established at all.
    Investigation empty = Start();
    Check(!InvestigationLoop::MayComplete(empty, refusal),
        "The runtime allowed completion with nothing established.");
    Check(refusal.find("nothing was actually established") != std::string::npos,
        "The refusal did not say that nothing was established: " + refusal);

    Check(report.outcome == InvestigationOutcome::Completed,
        "The ordinary case did not finish.");
}

// A model interpretation is never recorded as a tool observation.
void TestModelOutputIsNotPromotedToEvidence()
{
    Investigation investigation = Start();
    investigation.AddQuestion("Is the cache enabled?", 0.9, 0);

    InvestigationLoop loop;
    const RoundRunner runner = [&](const RoundRequest& request)
    {
        RoundResult result;
        CheckOutcome outcome = Observed(
            request.questions.front().id, QuestionStatus::Supported,
            "I think the cache is probably off", CheckKind::ModelReasoning);
        result.outcomes.push_back(outcome);
        result.proposeComplete = true;
        return result;
    };

    static_cast<void>(loop.Run(investigation, runner));
    Check(investigation.Findings().size() == 1, "The finding was not recorded.");
    Check(investigation.Findings().front().kind == EvidenceKind::ModelInterpretation,
        "The model's own reasoning was recorded as a tool observation.");
    Check(investigation.ObservationCount() == 0,
        "A round with no tool call reported a tool observation.");
    Check(investigation.FindingsSummary(1).find("[interpreted]") != std::string::npos,
        "The transcript did not distinguish interpretation from observation.");

    // And a finding with no check behind it is refused outright.
    Finding orphan;
    orphan.questionId = investigation.Questions().front().id;
    orphan.observed = "asserted without checking anything";
    Check(investigation.RecordFinding(orphan).empty(),
        "A finding with no check behind it was recorded.");
}

// Dependent checks wait for their inputs.
void TestDependenciesGateSelection()
{
    Investigation investigation = Start();
    const std::string first = investigation.AddQuestion("Which preset is active?", 0.9, 0);
    investigation.AddQuestion(
        "Does that preset set the launcher?", 0.95, 0, {first});

    // The dependent question has higher materiality, so only the dependency gate can be
    // what holds it back.
    const std::vector<InvestigationQuestion> selectable = investigation.Selectable();
    Check(selectable.size() == 1,
        "A question whose input was unsettled was offered for checking.");
    Check(selectable.front().id == first, "The wrong question was selectable.");

    investigation.SetQuestionStatus(first, QuestionStatus::Supported);
    Check(investigation.Selectable().size() == 1,
        "The dependent question did not become selectable once its input settled.");
    Check(investigation.Selectable().front().id != first,
        "The settled question was offered again.");
}

// The integrity rule: with no tools wired, a claim of having checked something is
// recorded as reasoning, never as an observation.
void TestClaimedToolUseIsDowngradedWithoutAnExecutor()
{
    RoundRequest request;
    request.round = 1;
    request.goal = "Why is the build slow?";
    InvestigationQuestion question;
    question.id = "q1";
    question.text = "Is the compiler cache enabled?";
    request.questions.push_back(question);

    // The model claims it ran the tests and read the logs. Nothing of the sort happened.
    const std::string raw =
        "FOUND q1 | supported | tests | ran the build twice and timed it | "
        "ccache is off in Release\n"
        "NEXT 0.8 | Which preset disables it?\n";

    const RoundResult withoutTools =
        InvestigationAgent::ParseRound(raw, request, /*checksAreAvailable=*/false);
    Check(withoutTools.outcomes.size() == 1, "The outcome was not parsed.");
    Check(withoutTools.outcomes.front().checkKind == CheckKind::ModelReasoning,
        "A claim of having run tests survived with no executor wired. The model must not "
        "be able to promote its own text into a tool observation.");
    Check(withoutTools.outcomes.front().checkDescription.find("no tools") !=
        std::string::npos,
        "The downgrade did not record that no tools were available.");
    Check(!withoutTools.outcomes.front().limitations.empty(),
        "The downgraded finding carried no limitation.");

    // Run it through the loop and confirm it lands as an interpretation.
    Investigation investigation(1, request.goal, "supported by evidence");
    investigation.AddQuestion(question.text, 0.9, 0);
    InvestigationLoop loop;
    const RoundRunner runner = [&](const RoundRequest& live)
    {
        return InvestigationAgent::ParseRound(
            "FOUND " + live.questions.front().id +
            " | supported | tests | ran them | ccache is off", live, false);
    };
    static_cast<void>(loop.Run(investigation, runner));
    Check(investigation.ObservationCount() == 0,
        "A round with no tools reported a tool observation.");

    // With an executor available the claim is allowed to stand as a check kind, because
    // something will actually run it.
    const RoundResult withTools =
        InvestigationAgent::ParseRound(raw, request, /*checksAreAvailable=*/true);
    Check(withTools.outcomes.front().checkKind == CheckKind::Tests,
        "With an executor wired the check kind was still downgraded.");
}

// Malformed field layouts do not become questions.
//
// Regression from a live run: the model wrote "NEXT | 0.6 | <question>" instead of
// "NEXT 0.6 | <question>", and the parser recorded a question whose entire text was
// "0.6". It occupied a slot in the investigation and appeared in the transcript as
// though she had asked it.
void TestMalformedFollowUpsAreNotRecordedAsQuestions()
{
    RoundRequest request;
    request.round = 1;
    InvestigationQuestion question;
    question.id = "q1";
    question.text = "Is the cache enabled?";
    request.questions.push_back(question);

    const std::string shifted =
        "NEXT | 0.6 | Does the release preset set the launcher?\n";
    const RoundResult result = InvestigationAgent::ParseRound(shifted, request, false);
    Check(result.followUps.size() == 1, "The shifted follow-up was dropped entirely.");
    Check(result.followUps.front().text == "Does the release preset set the launcher?",
        "The question text was taken from the wrong field: '" +
        result.followUps.front().text + "'");
    Check(result.followUps.front().materiality > 0.55 &&
        result.followUps.front().materiality < 0.65,
        "The score was not recognised in its shifted position.");

    // A bare number is never a question.
    const RoundResult bare =
        InvestigationAgent::ParseRound("NEXT 0.6 |\nNEXT | 0.9\n", request, false);
    Check(bare.followUps.empty(),
        "A follow-up with no question text was recorded anyway.");

    // The well-formed shape still works.
    const RoundResult ordinary = InvestigationAgent::ParseRound(
        "NEXT 0.8 | Which preset disables it?", request, false);
    Check(ordinary.followUps.size() == 1 &&
        ordinary.followUps.front().text == "Which preset disables it?",
        "The ordinary layout stopped parsing.");

    // And the opening parse has the same defence.
    const std::vector<ProposedQuestion> opening =
        InvestigationAgent::ParseOpeningQuestions("ASK | 0.7 | Is the cache on?", 4);
    Check(opening.size() == 1 && opening.front().text == "Is the cache on?",
        "The opening parse did not survive a shifted field layout.");
}

// The envelope carries earlier findings, which is what lets a later question use them.
void TestRoundEnvelopeCarriesEarlierFindings()
{
    RoundRequest request;
    request.round = 2;
    request.goal = "Why is the build slow?";
    InvestigationQuestion question;
    question.id = "q2";
    question.text = "Which preset disables it?";
    request.questions.push_back(question);

    Finding earlier;
    earlier.id = "f1";
    earlier.questionId = "q1";
    earlier.checkId = "c1";
    earlier.kind = EvidenceKind::ToolObservation;
    earlier.observed = "ccache is disabled in the release preset";
    earlier.limitations = "only the release preset was read";
    request.priorFindings.push_back(earlier);

    const std::string envelope =
        InvestigationAgent::BuildRoundEnvelope(request, "You are Revia.", false);
    Check(envelope.find("ccache is disabled in the release preset") != std::string::npos,
        "The round envelope did not carry the earlier finding, so a later question could "
        "not be chosen using it.");
    Check(envelope.find("only the release preset was read") != std::string::npos,
        "The finding's limitations were dropped on the way into the next round.");
    Check(envelope.find("NO tools") != std::string::npos,
        "The envelope did not tell the model it had no tools, which invites an invented "
        "check.");
    Check(envelope.find("Which preset disables it?") != std::string::npos,
        "The question being checked was not in the envelope.");
}

// Opening questions parse, and a malformed reply yields nothing rather than nonsense.
void TestOpeningQuestionParsing()
{
    const std::string raw =
        "ASK 0.9 | Is the compiler cache enabled?\n"
        "ASK 0.7 | How many units rebuild?\n"
        "I think the answer is probably caching.\n";
    const std::vector<ProposedQuestion> questions =
        InvestigationAgent::ParseOpeningQuestions(raw, 4);
    Check(questions.size() == 2, "Expected two questions, parsed " +
        std::to_string(questions.size()) + ".");
    Check(questions.front().materiality > 0.85, "Materiality did not parse.");
    Check(questions.front().text == "Is the compiler cache enabled?",
        "The question text did not parse.");

    Check(InvestigationAgent::ParseOpeningQuestions("no structure here", 4).empty(),
        "Prose with no ASK lines produced questions anyway.");
    Check(InvestigationAgent::ParseOpeningQuestions(raw, 1).size() == 1,
        "The question ceiling was not enforced.");
}

} // namespace

void RunInvestigationTests()
{
    TestMultipleQuestionsInOneRound();
    TestFindingProducesANewFollowUp();
    TestThreeRoundInvestigationReachesSupportedAnswer();
    TestContradictionChangesDirection();
    TestRepeatedQuestionsTriggerNoProgress();
    TestMissingEvidenceIsBlockedNotInvented();
    TestBudgetExhaustionPausesWithoutSuccess();
    TestCancellationRejectsLateResults();
    TestHidingSummariesDoesNotChangeBehaviour();
    TestSimpleTaskFinishesInOneRound();
    TestRuntimeRefusesPrematureCompletion();
    TestModelOutputIsNotPromotedToEvidence();
    TestDependenciesGateSelection();
    TestClaimedToolUseIsDowngradedWithoutAnExecutor();
    TestRoundEnvelopeCarriesEarlierFindings();
    TestOpeningQuestionParsing();
    TestMalformedFollowUpsAreNotRecordedAsQuestions();
    std::cout << "Investigation tests passed (17): the loop learns from findings, and "
                 "refuses to call a pause an answer.\n";
}
