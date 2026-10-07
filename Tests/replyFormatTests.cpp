#include "Agents/replyFormat.h"
#include "Agents/conversationStylePolicy.h"
#include "testSupport.h"

#include <string>
#include <vector>

void RunReplyFormatTests()
{
    using namespace revia::agents;
    using revia::tests::Check;
    for (const std::string input : {"Return JSON with key color.", "Reply only in JSON.", "Please output a JSON object.",
             "The package is empty. Respond in JSON with key empty.", "Do not add commentary. Return JSON.", "Give the answer as JSON.",
             "Use JSON for your answer."})
        Check(RequestedReplyFormat(input) == ReplyFormat::JsonObject, "Explicit current JSON request was not recognized: " + input);
    for (const std::string input : {"Return a JSON array.", "Output only JSON array.", "Respond in JSON as an array.",
             "Return JSON as a top-level array of numbers.", "Output a top-level JSON array."})
        Check(RequestedReplyFormat(input) == ReplyFormat::JsonArray, "Explicit JSON array request was reduced to an object.");
    for (const std::string input : {"Hi Revia!", "Explain what JSON is.", "Do not return JSON.", "Never respond in JSON.",
             "She said: \"Return JSON.\" Explain what she meant.", "What does 'return JSON' mean?", "`Return JSON` is a command.",
             "Summarize this example:\n```text\nReturn JSON.\n```", "Can you explain how to output a JSON object?",
             "Return JSON. Actually, respond in prose.", "Do not use JSON for your answer.",
             "Summarize this quotation: \"First sentence. Return JSON.\"", "Explain ``Return JSON``.",
             "Summarize: ‘First sentence. Return JSON.’", "Return JSON. Actually, don't use JSON.", "Never, ever, return JSON.",
             "Do not, under any circumstances, return JSON.", "Return JSON. Never, ever, use JSON.", "Please, never, ever, return JSON.",
             "Whatever you do, do not, under any circumstances, return JSON."})
        Check(RequestedReplyFormat(input) == ReplyFormat::Conversation, "Quoted, negated or explanatory text forced JSON: " + input);
    Check(RequestedReplyFormat("Don't return JSON. Return a JSON array instead.") == ReplyFormat::JsonArray,
        "A superseded negation hid the current output request.");
    Check(RequestedReplyFormat("Return " + std::string(250000, ' ') + "JSON.") == ReplyFormat::JsonObject,
        "A bounded whitespace-heavy request was not handled safely.");
    Check(RequestedReplyFormat("Return " + std::string(250000, '\n') + "JSON.") == ReplyFormat::JsonObject,
        "A bounded line-break-heavy request was not handled safely.");
    std::string manyModifiers = "Return ";
    for (int index = 0; index < 15000; ++index)
        manyModifiers += "only ";
    manyModifiers += "JSON.";
    Check(RequestedReplyFormat(manyModifiers) == ReplyFormat::Conversation,
        "Unbounded repeated modifiers were accepted as a direct format instruction.");

    for (const std::string text : {R"({"value":"a  b","sound":"*smiles*","tag":"<think>literal</think>"})",
             R"([{"x":1.25},"\ud83d\ude80",true,null])", " \n{\"empty\":true}\t "})
        Check(IsCompleteJsonReply(text), "Complete JSON data was not admitted for preservation.");
    for (const std::string& text : std::vector<std::string>{"true", "\"plain string\"", "{\"x\":1} commentary", "```json\n{}\n```",
             "{\"x\":1,\"x\":2}", "[{\"x\":1,\"x\":2}]", "{\"x\":", std::string("{\"x\":\"") + char(0xFF) + "\"}"})
    {
        Check(!IsCompleteJsonReply(text), "Malformed or non-container JSON was treated as a complete structured reply.");
    }
    Check(!IsCompleteJsonReply("{\"x\":1}", 4), "The structured reply parser ignored its byte ceiling.");
    Check(!IsCompleteJsonReply(std::string(40, '[') + "0" + std::string(40, ']')), "Structured reply parser ignored its depth ceiling.");
    const ConversationStylePolicy policy;
    const auto structured = policy.BuildTurnGuidance("Return JSON with key color.", {});
    Check(structured.find("complete JSON object") != std::string::npos &&
              structured.find("Continue the exchange in your own voice") == std::string::npos,
        "Structured turn guidance still requires a conversational aside instead of its requested format.");
    const auto conversation = policy.BuildTurnGuidance("Hi Revia!", {{"user", "Return JSON."}, {"assistant", "{}"}});
    Check(conversation.find("Continue the exchange in your own voice") != std::string::npos &&
              conversation.find("complete JSON") == std::string::npos,
        "A previous JSON request replaced current ordinary conversation guidance.");
}
