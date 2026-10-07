#include "Agents/replyFormat.h"
#include "Agents/conversationStylePolicy.h"
#include "testSupport.h"

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

void RunReplyContractTests()
{
    using namespace revia::agents;
    using revia::tests::Check;
    const auto keys = RequestedReplyContract("Return JSON with exactly keys empty and count.");
    Check(keys.extractionStatus == ReplyContractStatus::ExplicitShape, "Exact keys were not extracted into the turn contract.");
    const auto schema = nlohmann::json::parse(keys.schemaJson);
    Check(schema.at("properties").at("empty").empty() && schema.at("properties").at("count").empty(),
        "The key contract inferred value types from field names.");
    Check(MatchesReplyContract(R"({"empty":true,"count":0})", keys), "The exact-key contract refused matching data.");
    for (const std::string reply : {R"({"empty":true})", R"({"empty":true,"count":0,"extra":1})", R"({"Empty":true,"count":0})", "[]"})
        Check(!MatchesReplyContract(reply, keys), "The exact-key contract admitted a missing, extra or renamed key.");
    const auto quoted = RequestedReplyContract(R"(Return JSON with keys "camelCase", "ready".)");
    Check(quoted.extractionStatus == ReplyContractStatus::ExplicitShape &&
              MatchesReplyContract(R"({"camelCase":null,"ready":[]})", quoted) &&
              !MatchesReplyContract(R"({"camelcase":null,"ready":[]})", quoted),
        "Direct quoted field names lost their literal spelling.");
    const auto nested = RequestedReplyContract(
        R"(Return JSON using this schema: {"type":"object","properties":{"rows":{"type":"array","items":{"type":"object","properties":{"count":{"type":"integer"}},"required":["count"],"additionalProperties":false},"minItems":1,"maxItems":2}},"required":["rows"],"additionalProperties":false})");
    Check(nested.extractionStatus == ReplyContractStatus::ExplicitShape && MatchesReplyContract(R"({"rows":[{"count":2}]})", nested),
        "Explicit nested schema did not reach native validation.");
    for (const std::string reply :
        {R"({"rows":[]})", R"({"rows":[{"count":2.5}]})", R"({"rows":[{"count":"2"}]})", R"({"rows":[{"count":2,"extra":0}]})"})
        Check(!MatchesReplyContract(reply, nested), "Nested schema validation accepted an explicit constraint violation.");
    for (const std::string input : {"Return JSON with keys count and count.", "Return JSON with keys x and y, both integers.",
             R"(Return JSON using this schema: {"type":"object","$ref":"remote"})",
             R"(Return JSON using this schema: {"type":"object","type":"array"})",
             R"(Return JSON using this schema: {"type":"object","properties":{"x":{"type":"string","pattern":".*"}}})"})
    {
        const auto unsupported = RequestedReplyContract(input);
        Check(unsupported.extractionStatus == ReplyContractStatus::Unsupported && !unsupported.reason.empty() &&
                  MatchesReplyContract("{}", unsupported),
            "Unsupported explicit constraints did not diagnose and retain type-only handling.");
    }
    for (const std::string input :
        {R"(She said: "Return JSON with keys x and y." Explain.)", "Return JSON with keys x and y. Actually respond in prose.",
            "Never return JSON with keys x and y.", "Summarize this example:\n```\nReturn JSON with keys x and y.\n```"})
        Check(RequestedReplyContract(input).rootKind == ReplyFormat::Conversation, "Shape extraction altered format admission.");
    Check(RequestedReplyContract("Return JSON with keys x and y. Return JSON.").extractionStatus == ReplyContractStatus::TypeOnly,
        "A superseded key instruction contaminated the latest directive.");
    Check(MatchesReplyContract("[null,true,1]", RequestedReplyContract("Return JSON array.")) &&
              !MatchesReplyContract("{}", RequestedReplyContract("Return JSON array.")),
        "Type-only validation lost the root kind.");
    Check(RequestedReplyContract("Return " + std::string(250000, ' ') + "JSON.").rootKind == ReplyFormat::JsonObject,
        "Contract extraction exhausted or lost a whitespace-heavy admitted directive.");
    std::string manyKeys = "Return JSON with keys ";
    for (int index = 0; index < 65; ++index)
        manyKeys += (index ? ", " : "") + std::string("key") + std::to_string(index);
    Check(RequestedReplyContract(manyKeys).extractionStatus == ReplyContractStatus::Unsupported,
        "An explicit key list exceeded the 64-key limit.");
    auto deep = nlohmann::json{{"type", "string"}};
    for (int index = 0; index < 8; ++index)
        deep = {{"type", "array"}, {"items", deep}};
    Check(
        RequestedReplyContract("Return JSON array using this schema: " + deep.dump()).extractionStatus == ReplyContractStatus::Unsupported,
        "An explicit schema exceeded the eight-level limit.");
    Check(RequestedReplyContract("Return JSON using this schema: " + std::string(8200, ' ')).extractionStatus ==
              ReplyContractStatus::Unsupported,
        "An oversized explicit schema had no extraction diagnostic.");
    Check(RequestedReplyContract(std::string("Return JSON with keys \"") + char(0xFF) + "\".").extractionStatus ==
              ReplyContractStatus::Unsupported,
        "Malformed UTF-8 field names were admitted.");
    Check(!MatchesReplyContract(R"({"empty":true,"count":0,"count":1})", keys), "Duplicate output keys passed the contract validator.");
}

void RunReplyFormatTests()
{
    RunReplyContractTests();
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
