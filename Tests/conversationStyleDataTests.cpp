#include "Agents/conversationStylePolicy.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void Check(const bool value, const std::string& reason)
{
    if (!value)
        throw std::runtime_error(reason);
}

void TestIndirectEmotionQuestionsUseDigitalFeelingGuidance()
{
    const revia::agents::ConversationStylePolicy policy;
    for (const std::string question : {"How does this picture make you feel?", "HOW DOES THAT FEEDBACK MAKE YOU FEEL?",
             "How do those colors make you feel?", "How did that correction make you feel right now?"})
    {
        Check(policy.BuildTurnGuidance(question, {}).find("your own digital conversational feeling") != std::string::npos,
            "A direct make-you-feel question missed the existing digital feeling guidance: " + question);
        Check(!policy.CanStreamReply(question), "A direct feeling answer bypassed its existing whole-reply refinement gate.");
    }
    const std::string reaction = "It makes me curious about the contrast. I like the playful arrangement.";
    Check(policy.RefineReply("How does this picture make you feel?", {}, reaction) == reaction,
        "Recognizing the feeling question replaced an ordinary digital personal reaction.");
}

void TestEmotionQuestionCoveragePreservesCreativeAndQuotedTasks()
{
    const revia::agents::ConversationStylePolicy policy;
    for (const std::string request : {"Write a playful two-line poem about this picture.",
             "Write a fictional dialogue containing the question: How does this picture make you feel?",
             "Explain the grammar of the phrase: How does this picture make you feel?", "How does this picture make the traveler feel?",
             "How does this function make you feelgood?"})
        Check(policy.BuildTurnGuidance(request, {}).find("your own digital conversational feeling") == std::string::npos,
            "A creative, quoted, third-person or partial-word task was promoted to Revia's own feeling question: " + request);
    const std::string poem = "A red kite skips across the blue,\nA green hill keeps its joke for you.";
    Check(policy.RefineReply("Write a playful two-line poem about this picture.", {}, poem) == poem &&
              policy.CanStreamReply("Write a playful two-line poem about this picture."),
        "Emotion question coverage changed creative generation or its existing streaming policy.");
    Check(policy.BuildTurnGuidance("How does this picture make you feel? Return JSON with a feeling field.", {})
                  .find("your own digital conversational feeling") == std::string::npos,
        "Feeling guidance overrode an explicit structured reply contract.");
}

void TestPerFrameReportKeepsItsRepeatedFieldsAndTimestamps()
{
    const revia::agents::ConversationStylePolicy policy;
    const std::string report =
        "Here is the description of the sampled frames from the clip, strictly limited to the provided images.\n\n"
        "**Frame 1**\n*   **Timestamp:** 00:11.500\n*   **Label:** KITE42\n"
        "*   **Center:** A green triangle and a blue circle.\n\n"
        "**Frame 2**\n*   **Timestamp:** 00:11.900\n*   **Label:** KITE42\n"
        "*   **Center:** A green triangle and a blue circle.\n\n"
        "**Frame 3**\n*   **Timestamp:** 00:12.200\n*   **Label:** KITE42\n"
        "*   **Center:** A green triangle, a red square, and a blue circle.\n\n"
        "**Frame 4**\n*   **Timestamp:** 00:12.500\n*   **Label:** KITE42\n"
        "*   **Center:** A green triangle and a blue circle.\n\n"
        "**Limits of Sampling**\nThis analysis is based on four discrete frames sampled at the specific timestamps listed above. "
        "I cannot infer continuous motion, exact event timing, or the absence of unseen events between these sparse samples. "
        "There is no audio stream to reference.";
    Check(policy.RefineReply("Inspect the supplied video frames and report each sample.", {}, report) == report,
        "Conversational repetition repair erased per-frame data, changed timestamps or flattened the supplied report.");
}

void TestStructuredDataRepetitionIsNotConversationalReuse()
{
    const revia::agents::ConversationStylePolicy policy;
    const std::string reading = "The calibrated measurement remains at 0.75 units throughout this individual sample.";
    const std::string fields =
        "Sample A\nTimestamp: 00:01.125\nReading: " + reading + "\n\nSample B\nTimestamp: 00:02.875\nReading: " + reading;
    const std::string table = "| Sample | Timestamp | Measurement |\n| --- | --- | --- |\n| A | 00:01.125 | " + reading +
                              " |\n| B | 00:02.875 | " + reading + " |";
    const std::string numbered = "1. At 00:01.125, " + reading + "\n2. At 00:02.875, " + reading;
    const std::string bullets = "- " + reading + "\n- " + reading;
    const std::string code = "```text\n" + fields + "\n```";
    for (const auto& report : {fields, table, numbered, bullets, code})
    {
        const std::vector<conversationMessage> history = {{"assistant", report}};
        Check(policy.RefineReply("Repeat the measurements for each sample.", history, report) == report,
            "Structured readings were mistaken for repeated conversational sentences.");
        Check(policy.RefineReply("Correction: preserve the supplied measurement rows.", history, report) == report,
            "A correction erased requested data rows because the same values appeared in history.");
    }
    Check(policy.RefineReply("Report the samples.", {}, fields + "\nWhat's next?") == fields,
        "Preserving data rows disabled stock conversational tail removal.");
}

void TestProseRepairPreservesDecimalsAndStillRemovesDecodedLoops()
{
    const revia::agents::ConversationStylePolicy policy;
    const std::string decimal = "The measured gain is 0.75 with a calibrated baseline of 1.125 units.";
    Check(policy.RefineReply("Explain the measurement.", {}, decimal + ' ' + decimal) == decimal,
        "Prose repetition cleanup split decimal literals or stopped repairing a duplicated sentence.");
    const std::string sentence = "The cabinet remains safely closed during the entire careful inspection.";
    Check(policy.RefineReply("Explain the inspection.", {}, sentence + ' ' + sentence) == sentence,
        "Data preservation disabled ordinary substantial repetition repair.");
    const std::string block = "That part is useful. First repeated thought. Second repeated thought. ";
    Check(policy.RefineReply("Explain it.", {}, block + "First repeated thought. Second repeated thought.") ==
              "That part is useful. First repeated thought. Second repeated thought.",
        "Adjacent conversational sentence-block duplication stopped being repaired.");
    Check(policy.RefineReply("Explain it.", {}, "No! No! No!") == "No! No! No!", "Intentional short expressive repetition was erased.");
    Check(policy.RefineReply("Explain it.", {}, "The answer is ready. What's on your mind?") == "The answer is ready.",
        "The generic conversational tail stopped being removed.");
    const std::string json = R"({"value":0.75,"samples":[1.125,1.125],"literal":"a  b"})";
    Check(policy.RefineReply("Return JSON exactly.", {}, json) == json, "Literal JSON was changed by conversational data handling.");
}
}

void RunConversationStyleDataTests()
{
    TestIndirectEmotionQuestionsUseDigitalFeelingGuidance();
    TestEmotionQuestionCoveragePreservesCreativeAndQuotedTasks();
    TestPerFrameReportKeepsItsRepeatedFieldsAndTimestamps();
    TestStructuredDataRepetitionIsNotConversationalReuse();
    TestProseRepairPreservesDecimalsAndStillRemovesDecodedLoops();
    std::cout << "Conversation style structured data, decimal and prose-repetition checks passed.\n";
}

#ifdef REVIA_CONVERSATION_STYLE_DATA_TEST_MAIN
int main()
{
    try
    {
        RunConversationStyleDataTests();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#endif
