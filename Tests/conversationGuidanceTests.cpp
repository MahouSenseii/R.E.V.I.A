#include "testSupport.h"
#include "Agents/conversationStylePolicy.h"
#include "Agents/responseFilter.h"
#include "Identity/promptMarkers.h"

#include <iostream>
#include <string>

namespace
{
using revia::tests::Check;

bool HasCorrectionPurpose(const std::string& input)
{
    return revia::agents::ConversationStylePolicy{}.BuildTurnGuidance(input, {}).find("Check corrections against evidence;") !=
           std::string::npos;
}

void TestDirectCorrectionLabelsActivateCurrentPurpose()
{
    for (const std::string input : {"Correction: the rover has eight wheels, not six. What is its name and wheel count now?",
             "CORRECTION: Amber has eight wheels.", " \nCorrection: Amber has eight wheels.", "Correction: the rover has 'eight' wheels.",
             "Check your explanation against these supplied facts: the compiler produces object files; a static linker extracts needed "
             "object files from an archive and links an executable; a DLL remains separate, and its import library helps linking while the "
             "runtime loader loads the DLL. Correct any earlier error in three sentences.",
             "Check your explanation against these supplied facts: a compiler can produce object files for linking. Correct any earlier "
             "error in three sentences.",
             "Check your explanation against these supplied facts: a compiler can produce \"object files\" for linking. Correct any "
             "earlier error in three sentences.",
             "Check your explanation against these supplied facts: a compiler produces object files. Give the corrected explanation in "
             "exactly three sentences.",
             "Check your explanation against my earlier words. You misunderstood: the rover is Amber."})
        Check(HasCorrectionPurpose(input), "The actual direct Correction: wording did not activate current correction guidance.");
    Check(HasCorrectionPurpose("No, Amber has eight wheels."), "An existing direct correction signal stopped working.");
    Check(
        HasCorrectionPurpose("You misunderstood: the rover is Amber."), "An existing mistaken-interpretation correction stopped working.");
}

void TestCorrectionMentionsDoNotBecomeCurrentCorrection()
{
    for (const std::string input : {"She said: \"Correction: the rover has eight wheels.\"",
             "She said: Correction: the rover has eight wheels.", "\"Correction: the rover has eight wheels.\" is a quoted example.",
             "Correction: \"No, Amber has eight wheels.\" is a quoted example.", "He wrote: \"No, Amber has eight wheels.\"",
             "Do not treat this as a correction: it is a quotation example.", "No correction: the previous answer stands.",
             "This is not a correction: I am illustrating the label.", "I am not making a correction: I am showing the word.",
             "The word 'Correction:' is a label, not an instruction.", "Correction: is a label in this glossary.",
             "Correctional facilities are a different topic.",
             "\"Check your explanation against these supplied facts. Correct any earlier error.\" is a quoted example.",
             "She said: Check your explanation against these supplied facts. Correct any earlier error.",
             "Do not check your explanation against these supplied facts or correct any earlier error.",
             "Check your explanation against these supplied facts. Do not correct any earlier error.",
             "Check your explanation against these supplied facts: a compiler can produce object files for linking.",
             "Check this reference and correct any earlier error.",
             "Check your explanation against these supplied facts: the manual wrote: \"Correct any earlier error.\"",
             "Check your explanation against these supplied facts. Correct any earlier error is a label in this glossary.",
             "Check your explanation against these supplied facts. The phrase correct any earlier error is only a label."})
        Check(!HasCorrectionPurpose(input),
            "A quoted, reported, negated or metalinguistic correction mention became a direct correction: " + input);
}

void TestCorrectionGuidancePreservesEvidenceAndDisagreement()
{
    const auto guidance = revia::agents::ConversationStylePolicy{}.BuildTurnGuidance("No, two plus two is five.", {});
    Check(guidance.find("Check corrections against evidence; preserve disagreement and uncertainty.") != std::string::npos,
        "Correction guidance did not preserve evidence-based disagreement or uncertainty.");
    Check(guidance.find("attribute errors only when supported.") != std::string::npos &&
              guidance.find("Do not invent motives or blame.") != std::string::npos,
        "Correction guidance did not forbid invented blame or motives.");
    Check(guidance.find("Briefly accept the correction") == std::string::npos &&
              guidance.find("Do not defend, restate, or preserve the earlier assumption") == std::string::npos,
        "Correction guidance still required unconditional agreement.");
    const std::vector<conversationMessage> exchange = {
        {"user", "The rover is named Amber and has six wheels."}, {"assistant", "Amber is the rover with six wheels."}};
    const auto revisedDetail = revia::agents::ConversationStylePolicy{}.BuildTurnGuidance(
        "Correction: the rover has eight wheels, not six. What is its name and wheel count now?", exchange);
    const std::string expectedPurpose =
        "Turn-local conversation guidance: The latest message is the reply task. "
        "Check corrections against evidence; preserve disagreement and uncertainty. "
        "Keep unchanged facts and speaker ownership. A scenario revision is not evidence of your mistake. "
        "Preserve supplied relationships; attribute errors only when supported. Do not invent motives or blame.";
    Check(revisedDetail.substr(0, revisedDetail.find("\n\n")) == expectedPurpose && expectedPurpose.size() <= 450,
        "The correction purpose is not the complete compact first paragraph with evidence-conditional speaker repair.");
    Check(revisedDetail.find("Keep unchanged facts and speaker ownership.") != std::string::npos &&
              revisedDetail.find("Preserve supplied relationships;") != std::string::npos,
        "The native correction guidance omitted partial-revision continuity and supplied-fact reuse.");
    Check(revisedDetail.find("A scenario revision is not evidence of your mistake.") != std::string::npos &&
              revisedDetail.find("attribute errors only when supported.") != std::string::npos &&
              revisedDetail.find("repair your own error") == std::string::npos,
        "The native correction guidance inferred an assistant error from an ordinary user scenario revision.");
    Check(revisedDetail.find("Turn-local conversation guidance:", expectedPurpose.size()) == std::string::npos,
        "Lower correction advice introduced a second priority label.");
}

void TestCorrectionDoesNotReplaceAnUnrelatedWellbeingAnswer()
{
    revia::agents::ConversationStylePolicy policy;
    const std::string answer = "Amber has eight wheels. I'm feeling curious today.";
    for (const std::string input : {"Correction: the rover has eight wheels. How are you?", "No, the rover has eight wheels. How are you?"})
        Check(policy.RefineReply(input, {}, answer) == answer,
            "An unrelated correction and wellbeing question discarded the factual answer or invented a prior mood claim.");
    Check(policy.RefineReply("I'm not down. I was only asking how you are.", {}, "Right. I was just checking in. You're not down.") ==
              "Got it—you weren't saying you were down. I'm doing well.",
        "The specific mistaken-wellbeing repair stopped working.");
}

void TestCorrectionConsumersRespectTheAuthoredSignal()
{
    revia::agents::ConversationStylePolicy policy;
    Check(!policy.CanStreamReply("Correction: Amber has eight wheels."), "A direct correction was streamed before whole-reply refinement.");
    Check(policy.CanStreamReply("No correction: the previous answer stands."), "A negated correction mention disabled ordinary streaming.");
    const std::string repeated = "Amber is the synthetic rover whose six wheels were described in the previous answer.";
    const std::vector<conversationMessage> context = {{"assistant", repeated}};
    const std::string answer = repeated + " The corrected wheel count is eight.";
    Check(policy.RefineReply("Correction: Amber has eight wheels.", context, answer) == "The corrected wheel count is eight.",
        "A direct correction preserved a substantial sentence copied from the mistaken assistant history.");
    Check(policy.RefineReply("No correction: the previous answer stands.", context, answer) == answer,
        "A negated correction erased factual continuity as if the user had corrected it.");
}

void TestExistingAnswerModesRetainCompletenessAndTruth()
{
    using revia::agents::ConversationStylePolicy;
    const auto reliable = ConversationStylePolicy::BuildAnswerObligationGuidance(AnswerObligationMode::Reliable);
    const auto balanced = ConversationStylePolicy::BuildAnswerObligationGuidance(AnswerObligationMode::Balanced);
    const auto character = ConversationStylePolicy::BuildAnswerObligationGuidance(AnswerObligationMode::CharacterFirst);
    Check(reliable.find("Answer posture: reliable.") != std::string::npos &&
              reliable.find("do not stop short of the useful part") != std::string::npos,
        "Reliable answer completeness changed.");
    Check(balanced.find("Answer posture: balanced.") != std::string::npos &&
              balanced.find("a partial answer or a declined one") != std::string::npos,
        "Balanced answer freedom changed.");
    Check(character.find("Answer posture: character first.") != std::string::npos &&
              character.find("complete answer is optional") != std::string::npos,
        "Character-first answer freedom changed.");
    for (const auto& text : {reliable, balanced, character})
        Check(text.find("results the runtime actually confirmed") != std::string::npos &&
                  text.find("you may not invent one you were not given") != std::string::npos,
            "An answer mode stopped preserving confirmed outcomes.");
}

void TestCompleteJsonRetainsLiteralData()
{
    const revia::agents::ConversationStylePolicy policy;
    const revia::agents::ResponseFilter filter;
    const std::string repeated = "The cabinet remains safely closed during the entire careful inspection.";
    const std::string repeatedJson = "{\"text\":\"Intro. " + repeated + ' ' + repeated + " Outro.\"}";
    const std::vector<std::string> replies = {R"({"literal":"a  b", "tag":"[laugh]", "text":"*smiles*"})", repeatedJson,
        R"({"values":[3.1415,3.1415,-0.02],"nested":{"text":"a  b","tag":"*sighs*"}})", R"(["[laugh]","*smiles*","a  b",{"value":1.001}])",
        " \n{\"text\":\"a  b\"}\n "};
    for (const auto& reply : replies)
    {
        const std::vector<conversationMessage> prior = {{"assistant", reply}};
        Check(policy.RefineReply("Correction: return the JSON exactly.", prior, reply) == reply,
            "Conversational style changed a complete JSON value or its intentional repetition.");
        const auto result = filter.ApplyHard("Return JSON exactly.", reply, {}, 12000);
        Check(result.text == reply && !result.changed && !result.blocked,
            "The hard filter changed a literal JSON value or its permitted whitespace.");
    }
}

void TestJsonRetainsHardSafetyChecks()
{
    const revia::agents::ResponseFilter filter;
    const std::string marker = "{\"text\":\"" + std::string(revia::identity::markers::RuntimeTurnContext) + "\"}";
    Check(filter.ApplyHard("Return JSON.", marker, {}, 12000).blocked, "A JSON envelope bypassed the hidden prompt marker guard.");
    revia::agents::ResponseFilterContext context;
    context.desktopStateKnown = true;
    Check(filter.ApplyHard("Return JSON.", R"({"claim":"I'm looking at your screen."})", context, 12000).blocked,
        "A JSON envelope bypassed the unobserved screen claim guard.");
    Check(filter.ApplyHard("Return JSON.", R"({"claim":"I clicked the button."})", context, 12000).blocked,
        "A JSON envelope bypassed desktop authority grounding.");
    const std::string malformedUtf8 = std::string("{\"text\":\"") + static_cast<char>(0xFF) + "\"}";
    Check(filter.ApplyHard("Return JSON.", malformedUtf8, {}, 12000).blocked, "A JSON envelope bypassed UTF-8 validation.");
}

void TestJsonLimitsRefuseInsteadOfCuttingData()
{
    const revia::agents::ConversationStylePolicy policy;
    const revia::agents::ResponseFilter filter;
    for (const auto size : {std::size_t{400}, std::size_t{262145}})
    {
        const std::string oversized = "{\"text\":\"" + std::string(size, 'x') + "\"}";
        const auto result = filter.ApplyHard("Return JSON.", oversized, {}, 256);
        Check(result.blocked && result.changed && result.text.size() <= 256 && result.reason.find("JSON") != std::string::npos,
            "The hard filter silently cut oversized JSON instead of reporting a bounded refusal.");
    }
    std::string repeated;
    while (repeated.size() <= 262144)
        repeated += "This complete sentence is intentionally repeated as part of the supplied data. ";
    const std::string oversized = "{\"text\":\"" + repeated + "\"}";
    Check(policy.RefineReply("Return JSON.", {}, oversized) == oversized,
        "Style cleanup silently reduced JSON above its parse bound before the hard filter could refuse it.");
}

void TestJsonControlTokensFailInsteadOfChangingData()
{
    const revia::agents::ResponseFilter filter;
    for (const std::string token : {"<|im_start|>", "<|im_end|>", "<|endoftext|>", "<|assistant|>", "<|user|>", "[INST]", "[/INST]"})
    {
        for (const std::string candidate : {"{\"nested\":{\"text\":\"before" + token + "after\"}}", "[\"before" + token + "after\"]"})
        {
            const auto result = filter.ApplyHard("Return JSON exactly.", candidate, {}, 256);
            Check(result.blocked && result.changed && result.text.size() <= 256 && result.text.find(token) == std::string::npos &&
                      result.reason.find("control") != std::string::npos && !result.text.starts_with('{') && !result.text.starts_with('['),
                "The hard filter silently changed a JSON value containing a disallowed control token instead of refusing it.");
        }
        const auto prose = filter.ApplyHard("Explain the text.", "before" + token + "after", {}, 256);
        Check(prose.text == "beforeafter" && prose.changed && !prose.blocked,
            "JSON control-token refusal changed the existing prose erasure policy.");
    }
}

void TestOrdinaryAndMalformedRepliesKeepPresentationChecks()
{
    const revia::agents::ConversationStylePolicy policy;
    const revia::agents::ResponseFilter filter;
    const std::string sentence = "The cabinet remains safely closed during the entire careful inspection.";
    Check(policy.RefineReply("Explain the inspection.", {}, sentence + ' ' + sentence) == sentence,
        "Preserving JSON disabled prose repetition cleanup.");
    const auto ordinary = filter.ApplyHard("Say hello.", "Hello  there. *smiles* [laugh]", {}, 12000);
    Check(ordinary.text == "Hello there. *laughs*" && ordinary.changed && !ordinary.blocked,
        "Preserving JSON disabled ordinary vocalization and stage direction cleanup.");
    for (const std::string malformed :
        {R"({not-json *smiles*)", R"({"text":"*smiles*"} trailing)", R"({"text":"*smiles*","text":"another"})"})
    {
        const auto result = filter.ApplyHard("Return JSON.", malformed, {}, 12000);
        Check(result.changed && result.text.find("*smiles*") == std::string::npos,
            "Incomplete, trailing or duplicate-key JSON bypassed ordinary presentation checks.");
    }
}
}

void RunConversationGuidanceTests()
{
    TestDirectCorrectionLabelsActivateCurrentPurpose();
    TestCorrectionMentionsDoNotBecomeCurrentCorrection();
    TestCorrectionGuidancePreservesEvidenceAndDisagreement();
    TestCorrectionDoesNotReplaceAnUnrelatedWellbeingAnswer();
    TestCorrectionConsumersRespectTheAuthoredSignal();
    TestExistingAnswerModesRetainCompletenessAndTruth();
    TestCompleteJsonRetainsLiteralData();
    TestJsonRetainsHardSafetyChecks();
    TestJsonLimitsRefuseInsteadOfCuttingData();
    TestJsonControlTokensFailInsteadOfChangingData();
    TestOrdinaryAndMalformedRepliesKeepPresentationChecks();
    std::cout
        << "Direct correction guidance, evidence-based disagreement, non-correction mentions and configured answer freedom tests passed.\n";
}
