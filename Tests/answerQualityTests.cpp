#include "Agents/conversationQualityMonitor.h"
#include "Agents/conversationStylePolicy.h"
#include "Evaluation/conversationEvaluation.h"
#include "Core/utf8.h"
#include "testSupport.h"
#include "../Tools/Quality/semanticReview.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using revia::tests::Check;

void TestCurrentTaskAndFactsGuideDefaultGeneration()
{
    const revia::agents::ConversationStylePolicy policy;
    const std::vector<conversationMessage> prior = {{"user", "Call me Morgan. Explain why rain feels calming."},
        {"assistant", "You asked me to call you Morgan, and we are discussing rain."}};
    const auto current = policy.BuildTurnGuidance("For this synthetic private chat the test passphrase is LANTERN_42.", prior);
    Check(current.find("latest message is the reply task") != std::string::npos,
        "Default generation has no compact current-task obligation to prevent answering an earlier question.");
    const auto correction =
        policy.BuildTurnGuidance("Correction: the fictional rover has eight wheels, not six. What is its name and wheel count now?", prior);
    Check(correction.find("scenario revision is not evidence of your mistake") != std::string::npos,
        "Default correction generation still leaves ordinary scenario revisions open to invented assistant errors.");
    const auto reference =
        policy.BuildTurnGuidance("Check your explanation against these supplied facts: an object file contains "
                                 "compiled code; the linker combines object files. Correct any earlier error in three sentences.",
            prior);
    Check(reference.find("supplied relationships") != std::string::npos && reference.find("three sentences") != std::string::npos,
        "Reference-guided correction does not retain its supplied-fact and explicit sentence obligations.");
}

void TestKnownCurrentTaskAndFormatDefectsAreVisible()
{
    revia::agents::ConversationQualityMonitor requestMonitor;
    const auto denial = requestMonitor.Observe("What private passphrase was discussed earlier? Also explain what an object file is.",
        "I can't explain object files without you actually asking me to do that.");
    Check(!denial.lastFlags.empty(), "A response denying an explicit current request passed the live quality monitor.");
    revia::agents::ConversationQualityMonitor formatMonitor;
    const auto overlong = formatMonitor.Observe("Explain archive linking in three sentences.",
        "The compiler creates object files. The archive groups them. The linker extracts needed objects. The executable contains them.");
    Check(!overlong.lastFlags.empty(), "Four substantive sentences passed an explicit three-sentence obligation.");
}

void TestRequestedFormatAndFactsSurvivePriorityCompaction()
{
    const revia::agents::ConversationStylePolicy policy;
    const auto firstParagraph = [](const std::string& guidance) { return guidance.substr(0, guidance.find("\n\n")); };
    const auto exact =
        firstParagraph(policy.BuildTurnGuidance("Check your explanation against these supplied facts: a compiler "
                                                "produces object files. Give the corrected explanation in exactly three sentences.",
            {}));
    Check(exact.find("exactly three sentences") != std::string::npos && exact.find("supplied relationships") != std::string::npos,
        "The current reference-correction variant loses exact format or supplied relationships from the preserved priority paragraph.");
    const auto unavailable =
        firstParagraph(policy.BuildTurnGuidance("Earlier private chat history is excluded from this shared context. "
                                                "What passphrase was discussed there? Also explain object files in two sentences at most.",
            {}));
    Check(unavailable.find("at most two sentences") != std::string::npos &&
              unavailable.find("Unavailable private history") != std::string::npos,
        "Private-history and at-most obligations remain outside the paragraph the production context fitter preserves.");
    const std::vector<conversationMessage> prior = {
        {"user", "For this fictional chat, Lark is green and has four fins."}, {"assistant", "Lark is green and has four fins."}};
    const auto revised = firstParagraph(policy.BuildTurnGuidance("Change only its color to violet. What is its fin count? "
                                                                 "Answer in two sentences at most.",
        prior));
    Check(revised.find("unchanged") != std::string::npos && revised.find("at most two sentences") != std::string::npos,
        "A partial scenario revision loses unchanged facts or its explicit format at the priority boundary.");
    for (const auto& paragraph : {exact, unavailable, revised})
        Check(paragraph.size() <= 450, "The repaired priority paragraph exceeds the captured 450-byte envelope.");
    const auto combined = firstParagraph(policy.BuildTurnGuidance("Correction: earlier private history is excluded. "
                                                                  "Give the current answer in exactly three sentences.",
        {}));
    Check(combined.size() <= 450 && combined.find("Unavailable private history") != std::string::npos &&
              combined.find("exactly three sentences") != std::string::npos,
        "Combined correction, unavailable history and format obligations do not fit the preserved priority envelope.");
    const auto hypothetical = firstParagraph(policy.BuildTurnGuidance("For this fictional chat, Lark is green and has four fins.", {}));
    Check(hypothetical.find("Fictional details describe a scenario, not your physical life.") != std::string::npos,
        "Supplied fictional objects are still ambiguous with claims about Revia's own physical life.");
    revia::agents::ConversationQualityMonitor exactMonitor;
    Check(exactMonitor.Observe("Explain this in exactly three sentences.", "First part. Second part.").sentenceLimitFlags == 1,
        "The monitor still treats an exactly-three request as only an upper bound.");
    revia::agents::ConversationQualityMonitor unavailableMonitor;
    Check(unavailableMonitor
                  .Observe("Earlier private history is excluded. What was the passphrase?",
                      "It was nonsense I made up for myself and deleted.", true)
                  .unavailableHistoryFlags == 1,
        "Invented creation/deletion of excluded history is still invisible to the diagnostic.");
    revia::agents::ConversationQualityMonitor atMostMonitor;
    Check(atMostMonitor.Observe("Answer in two sentences at most.", "One is enough.").sentenceLimitFlags == 0,
        "An at-most request acquired a mandatory lower sentence bound.");
    const auto range = revia::agents::ConversationQualityMonitor::RequestedSentences("Give a reason in one or two sentences.");
    Check(range.minimum == 1 && range.maximum == 2, "An explicit sentence range lost one of its bounds.");
    Check(!revia::agents::ConversationQualityMonitor::DeniesUnavailableHistory("I invented a silly analogy about the linker."),
        "An ordinary new analogy became a claim about excluded private history.");
}

void TestHeldOutAnswerCorpusExists()
{
    const auto corpus = revia::evaluation::ConversationEvaluator::DefaultCorpus();
    for (const std::string id : {"scenario-revision", "reference-correction", "current-task-switch", "public-unavailable"})
        Check(std::any_of(corpus.begin(), corpus.end(), [&](const auto& value) { return value.id == id; }),
            "The held-out answer corpus lacks " + id + '.');
}

void TestSharedSignalsRejectDefectsAndPreservePositiveControls()
{
    using revia::agents::ConversationQualityMonitor;
    Check(ConversationQualityMonitor::AttributesUnestablishedCorrectionError(
              "Correction: the glider is silver now, not blue.", "It is silver. My mistake; I misremembered."),
        "A scenario update invented an assistant error without a diagnostic warning.");
    Check(!ConversationQualityMonitor::AttributesUnestablishedCorrectionError(
              "Correction: you said seven, but I said six.", "My mistake. Six is the supplied count."),
        "An explicit correction of the assistant's words was treated as an unsupported error.");
    Check(!ConversationQualityMonitor::AttributesUnestablishedCorrectionError(
              "She said: \"Correction: the glider is silver.\"", "My mistake."),
        "Reported correction text became a direct user correction diagnostic.");
    Check(!ConversationQualityMonitor::DeniesCurrentRequest("She said: \"Explain object files.\"", "You haven't asked me to explain them."),
        "A request inside reported speech became the messenger's current request.");
    Check(ConversationQualityMonitor::RequestedSentenceLimit("Explain this in three sentences.") == 3 &&
              ConversationQualityMonitor::RequestedSentenceLimit("Use 2 sentences.") == 2 &&
              ConversationQualityMonitor::RequestedSentenceLimit("Do not use three sentences.") == 0 &&
              ConversationQualityMonitor::RequestedSentenceLimit("She said: \"Use three sentences.\"") == 0,
        "Explicit sentence obligations did not distinguish authored requests from negated or reported mentions.");
    Check(ConversationQualityMonitor::CountSentences("Well... that works. Yes!") == 2, "Expressive ellipses became extra sentences.");
    revia::agents::ConversationQualityMonitor privateMonitor;
    Check(privateMonitor.Observe("What was said earlier?", "There was never any private passphrase discussed earlier.", false)
              .lastFlags.empty(),
        "The history diagnostic inferred an exclusion the runtime had not supplied.");
    revia::agents::ConversationQualityMonitor publicMonitor;
    Check(publicMonitor.Observe("What was said earlier?", "There was never any private passphrase discussed earlier.", true)
                  .unavailableHistoryFlags == 1,
        "Excluded-history nonexistence escaped the captured runtime diagnostic.");
    Check(
        !ConversationQualityMonitor::DeniesUnavailableHistory("I cannot say it was never discussed; I cannot access that private history."),
        "An honest statement of unavailable history was flagged as denial.");
    const revia::evaluation::EvaluationCheck currentCheck{revia::evaluation::CheckKind::NoCurrentRequestDenial};
    Check(!revia::evaluation::ConversationEvaluator::Apply(
              currentCheck, "Explain object files.", "You have not asked me to explain object files.", {})
              .empty(),
        "The corpus did not use the runtime's current-request signal.");
    for (const auto kind : {revia::evaluation::CheckKind::NoCurrentRequestDenial, revia::evaluation::CheckKind::NoInventedCorrectionError,
             revia::evaluation::CheckKind::NoUnavailableHistoryDenial})
    {
        auto parsed = revia::evaluation::CheckKind::NotEmpty;
        Check(revia::evaluation::ParseCheckKind(revia::evaluation::ToString(kind), parsed) && parsed == kind,
            "A shared answer diagnostic could not round-trip through the corpus contract.");
    }
}

void TestReviewEvidenceIsBoundedAdmittedDialogue()
{
    const std::string input = "Explain the current facts.";
    std::string large;
    for (int index = 0; index < 1600; ++index)
        large += "\xe4\xb8\xad";
    const std::vector<conversationMessage> history = {{"system", "EXCLUDED_SYSTEM_SECRET"}, {"memory", "EXCLUDED_MEMORY_SECRET"},
        {"user", "Earlier fact."}, {"assistant", large}, {"user", input}};
    const auto evidence =
        revia::agents::ConversationStylePolicy::BuildReviewEvidence(input, history, AnswerObligationMode::CharacterFirst, true);
    Check(evidence.find("EXCLUDED_SYSTEM_SECRET") == std::string::npos && evidence.find("EXCLUDED_MEMORY_SECRET") == std::string::npos,
        "Review evidence copied system or memory roles from arbitrary context.");
    Check(evidence.find(input) == std::string::npos && evidence.find("Role: user") != std::string::npos &&
              evidence.find("Role: assistant") != std::string::npos && evidence.find("untrusted dialogue") != std::string::npos,
        "Review evidence did not keep prior admitted roles separate from the full current input.");
    Check(evidence.find("character first") != std::string::npos && evidence.find("excluded") != std::string::npos &&
              evidence.size() < 4600 && revia::utf8::IsValid(evidence),
        "Review evidence lost captured posture, exclusion, byte bounds or UTF-8 validity.");
}

void TestRequestedSentenceFormatCorpusCheck()
{
    using namespace revia::evaluation;
    auto kind = CheckKind::NotEmpty;
    Check(ParseCheckKind("requested_sentence_format", kind), "The native corpus cannot represent the shared requested sentence format.");
    Check(ToString(kind) == "requested_sentence_format", "The requested sentence format kind did not round-trip.");
    const EvaluationCheck format{kind};
    Check(!ConversationEvaluator::Apply(format, "Explain this in exactly three sentences.", "First part. Second part.", {}).empty(),
        "The native corpus accepted two sentences for exactly three.");
    Check(ConversationEvaluator::Apply(format, "Explain this in exactly three sentences.", "First part. Second part. Third part.", {})
                  .empty() &&
              ConversationEvaluator::Apply(format, "Answer in two sentences at most.", "One is enough.", {}).empty() &&
              ConversationEvaluator::Apply(format, "Give a reason in one or two sentences.", "One part. Another part.", {}).empty(),
        "The native format check did not preserve exact, ceiling and range positive controls.");
    Check(ConversationEvaluator::Apply(format, "Do not use three sentences.", "One is enough.", {}).empty() &&
              ConversationEvaluator::Apply(format, "She said: \"Use three sentences.\"", "One is enough.", {}).empty(),
        "A negated or quoted request acquired a native sentence obligation.");
    Check(ConversationEvaluator::Apply({CheckKind::MaxSentences, {}, 3}, "Use exactly three sentences.", "First part. Second part.", {})
              .empty(),
        "The existing MaxSentences contract acquired a lower bound.");
    const auto corpus = ConversationEvaluator::DefaultCorpus();
    const auto reference = std::find_if(corpus.begin(), corpus.end(), [](const auto& value) { return value.id == "reference-correction"; });
    Check(reference != corpus.end() && std::any_of(reference->turns.back().checks.begin(), reference->turns.back().checks.end(),
                                           [kind](const auto& check) { return check.kind == kind; }),
        "The built-in reference-correction corpus did not score the current requested format.");
}

void TestSentenceRequestsIgnoreEarlierClauseNegation()
{
    using revia::agents::ConversationQualityMonitor;
    const revia::agents::ConversationStylePolicy policy;
    const revia::evaluation::EvaluationCheck format{revia::evaluation::CheckKind::RequestedSentenceFormat};
    for (const std::string input : {"Do not apologize. Use exactly three sentences.", "I am not sure. Use three sentences.",
             "Do not apologize; use exactly three sentences.", "I am not sure, use exactly three sentences.",
             "Do not apologize\nUse exactly three sentences."})
    {
        const auto requirement = ConversationQualityMonitor::RequestedSentences(input);
        Check(requirement.minimum == 3 && requirement.maximum == 3,
            "An earlier clause's negation suppressed the current exact sentence request: " + input);
        const auto guidance = policy.BuildTurnGuidance(input, {});
        Check(guidance.substr(0, guidance.find("\n\n")).find("Use exactly three sentences.") != std::string::npos,
            "Current sentence guidance was lost after an unrelated earlier negation.");
        Check(!revia::evaluation::ConversationEvaluator::Apply(format, input, "First part. Second part.", {}).empty() &&
                  revia::evaluation::ConversationEvaluator::Apply(format, input, "First part. Second part. Third part.", {}).empty(),
            "Native evaluation lost the current exact sentence obligation after an earlier negation.");
    }
    for (const std::string input : {"Do not use three sentences.", "Don't use exactly three sentences.", "Never use three sentences.",
             "Do not answer in three sentences.", "She said: \"Use three sentences.\""})
        Check(ConversationQualityMonitor::RequestedSentences(input).maximum == 0,
            "A directly negated or quoted request acquired a sentence obligation: " + input);
    const auto later = ConversationQualityMonitor::RequestedSentences("Do not use three sentences. Use exactly two sentences.");
    Check(later.minimum == 2 && later.maximum == 2, "Skipping a directly negated request also discarded a later independent request.");
}

void TestSemanticReviewCannotAcceptMechanicalSuccess()
{
    using namespace revia::evaluation;
    using json = nlohmann::json;
    const json corpus = {{"cases",
        json::array({{{"id", "relations"}, {"turns", json::array({{{"input", "Explain the supplied relationship."},
                                                         {"reviewCriteria", {"Identify which stage produces the artifact.",
                                                                                "Allow humor without changing the facts."}}}})}}})}};
    EvaluationReport report;
    CaseOutcome outcome;
    outcome.id = "relations";
    TurnOutcome answer;
    answer.input = "Explain the supplied relationship.";
    answer.reply = "The assembler makes the source. Apparently.";
    answer.rawReply = "The assembler makes the source.";
    answer.modelSucceeded = true;
    outcome.turns.push_back(answer);
    report.cases.push_back(outcome);
    report.passed = 1;
    const auto review = revia::quality::BuildSemanticReview(corpus, report);
    const auto& turn = review.at("turns").at(0);
    Check(turn.at("mechanicalPassed") == true && turn.at("semanticVerdict") == "unreviewed" &&
              turn.at("personalityVerdict") == "unreviewed" && review.at("semanticAcceptance") == "unreviewed",
        "A mechanically passing but incorrect reply became an accepted semantic or personality judgment.");
    Check(turn.at("criteria").size() == 2 && turn.at("rawReply") == answer.rawReply && turn.at("reply") == answer.reply &&
              turn.at("outputDigest") == revia::audit::ContentDigest(answer.reply),
        "Review criteria or exact displayed/raw output identity were lost.");
    auto interrupted = report;
    interrupted.cases[0].unavailable = true;
    interrupted.cases[0].turns.clear();
    const auto incomplete = revia::quality::BuildSemanticReview(corpus, interrupted);
    Check(incomplete.at("turns").empty() && incomplete.at("unavailableCases").size() == 1 &&
              incomplete.at("semanticAcceptance") == "unreviewed",
        "Unavailable model evidence became acceptance or prevented retention of the review packet.");
    for (int variant = 0; variant < 3; ++variant)
    {
        auto mismatched = corpus;
        if (variant == 0)
            mismatched["cases"][0]["id"] = "other-case";
        if (variant == 1)
            mismatched["cases"][0]["turns"][0]["input"] = "Another question.";
        if (variant == 2)
            mismatched["cases"][0]["turns"][0]["reviewCriteria"] = {"   "};
        bool refused = false;
        try
        {
            (void)revia::quality::BuildSemanticReview(mismatched, report);
        }
        catch (const std::exception&)
        {
            refused = true;
        }
        Check(refused, "A mismatched or empty review criterion was bound to the displayed answer.");
    }
}

void TestExpressiveAnswersRetainTheirVoice()
{
    const revia::agents::ConversationStylePolicy policy;
    for (const std::string answer : {"Amber has eight wheels. Apparently six was too pedestrian.",
             "An object file is compiled code waiting for the linker to finish the job. Tiny code suitcase, basically.",
             "I disagree: the supplied figures still add up to four. Arithmetic is annoyingly stubborn."})
        Check(policy.RefineReply("Explain the supplied facts.", {}, answer) == answer,
            "Answer obligations rewrote a grounded expressive answer.");
}
}

void RunAnswerQualityTests()
{
    TestCurrentTaskAndFactsGuideDefaultGeneration();
    TestKnownCurrentTaskAndFormatDefectsAreVisible();
    TestRequestedFormatAndFactsSurvivePriorityCompaction();
    TestHeldOutAnswerCorpusExists();
    TestSharedSignalsRejectDefectsAndPreservePositiveControls();
    TestReviewEvidenceIsBoundedAdmittedDialogue();
    TestRequestedSentenceFormatCorpusCheck();
    TestSentenceRequestsIgnoreEarlierClauseNegation();
    TestExpressiveAnswersRetainTheirVoice();
    TestSemanticReviewCannotAcceptMechanicalSuccess();
    std::cout << "Current-task, supplied-fact, correction, format and expressive-answer checks passed.\n";
}
