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
    const std::string introducedSchema =
        R"({"type":"object","properties":{"thermalRatio":{"type":"number"},"enabledFlag":{"type":"boolean"}},"required":["thermalRatio","enabledFlag"],"additionalProperties":false})";
    for (const std::string lead : {"Follow this JSON Schema: ", "Use this JSON Schema: "})
        for (const std::string tail : {"", ". Return only valid JSON, without extra commentary.",
                 R"(. Return JSON with exactly keys "enabledFlag", "thermalRatio". Return only valid JSON.)"})
        {
            const auto introduced = RequestedReplyContract(lead + introducedSchema + tail);
            Check(introduced.extractionStatus == ReplyContractStatus::ExplicitShape &&
                      nlohmann::json::parse(introduced.schemaJson) == nlohmann::json::parse(introducedSchema) &&
                      MatchesReplyContract(R"({"thermalRatio":2.75,"enabledFlag":true})", introduced) &&
                      !MatchesReplyContract(R"({"thermalRatio":"2.75","enabledFlag":true})", introduced),
                "A direct schema introduction or compatible reaffirmation lost explicit types: " + lead + tail);
        }
    const auto introducedArray = RequestedReplyContract(
        R"(Follow this JSON Schema: {"type":"array","items":{"type":"number"},"minItems":1,"maxItems":2}. Return only valid JSON array, without extra commentary.)");
    Check(introducedArray.rootKind == ReplyFormat::JsonArray && introducedArray.extractionStatus == ReplyContractStatus::ExplicitShape &&
              MatchesReplyContract("[2.75]", introducedArray) && !MatchesReplyContract("[true]", introducedArray),
        "A direct array-schema introduction lost its root or bounded item types.");
    const auto bareArrayDelivery = RequestedReplyContract(
        R"(Use this JSON Schema: {"type":"array","items":{"type":"number"},"minItems":1,"maxItems":2}. Return only valid JSON. Return only JSON, without extra commentary.)");
    Check(bareArrayDelivery.rootKind == ReplyFormat::JsonArray &&
              bareArrayDelivery.extractionStatus == ReplyContractStatus::ExplicitShape &&
              MatchesReplyContract("[2.75]", bareArrayDelivery) && !MatchesReplyContract("{}", bareArrayDelivery),
        "A bare JSON delivery reaffirmation silently replaced an intrinsic array-schema root.");
    const auto legacyArrayDelivery = RequestedReplyContract(
        R"(Return JSON using this schema: {"type":"array","items":{"type":"number"},"minItems":1,"maxItems":2}. Return only valid JSON.)");
    Check(legacyArrayDelivery.rootKind == ReplyFormat::JsonArray &&
              legacyArrayDelivery.extractionStatus == ReplyContractStatus::ExplicitShape &&
              MatchesReplyContract("[2.75]", legacyArrayDelivery) && !MatchesReplyContract("{}", legacyArrayDelivery),
        "A legacy schema introduction or bare delivery lost its declared array root.");
    for (const std::string lead : {"Use this JSON Schema: ", "Return JSON using this schema: "})
    {
        const auto mixedArrayDelivery = RequestedReplyContract(
            lead + R"({"type":"array","items":{"type":"number"}}. Return only valid JSON array. Return only valid JSON.)");
        Check(mixedArrayDelivery.rootKind == ReplyFormat::JsonArray &&
                  mixedArrayDelivery.extractionStatus == ReplyContractStatus::ExplicitShape &&
                  MatchesReplyContract("[2.75]", mixedArrayDelivery) && !MatchesReplyContract("[true]", mixedArrayDelivery),
            "Same-kind explicit array delivery followed by bare JSON lost the declared array schema.");
    }
    const auto conflictingRoot =
        RequestedReplyContract(R"(Return JSON object using this schema: {"type":"array","items":{"type":"number"}}.)");
    Check(conflictingRoot.rootKind == ReplyFormat::JsonObject && conflictingRoot.extractionStatus == ReplyContractStatus::Unsupported,
        "A schema root overrode an explicitly incompatible current container request.");
    const auto unsupportedArray = RequestedReplyContract(
        R"(Use this JSON Schema: {"type":"array","items":{"type":"string","pattern":".*"}}. Return only valid JSON.)");
    Check(unsupportedArray.rootKind == ReplyFormat::JsonArray && unsupportedArray.extractionStatus == ReplyContractStatus::Unsupported &&
              !unsupportedArray.reason.empty() && unsupportedArray.schemaJson == R"({"type":"array"})" &&
              MatchesReplyContract("[true]", unsupportedArray) && !MatchesReplyContract("{}", unsupportedArray),
        "Unsupported introduced array constraints lost their known root or diagnostic instead of falling back to type-only.");
    const auto malformedIntroduced =
        RequestedReplyContract(std::string(R"(Follow this JSON Schema: {"type":"array","items":{"type":")") + char(0xFF) + R"("}})");
    Check(malformedIntroduced.extractionStatus == ReplyContractStatus::Unsupported && !malformedIntroduced.reason.empty(),
        "Malformed UTF-8 in a schema introduction threw or lacked a conservative extraction diagnostic.");
    const auto overriddenArray = RequestedReplyContract(
        R"(Follow this JSON Schema: {"type":"array","items":{"type":"number"}}. Return a JSON object. Return only valid JSON.)");
    Check(overriddenArray.rootKind == ReplyFormat::JsonObject && overriddenArray.extractionStatus == ReplyContractStatus::TypeOnly &&
              MatchesReplyContract("{}", overriddenArray) && !MatchesReplyContract("[2.75]", overriddenArray),
        "A later explicit object override resurrected an earlier array schema.");
    for (const std::string input : {"Never follow this JSON Schema: " + introducedSchema,
             "Do not use this JSON Schema: " + introducedSchema, "Explain how to use this JSON Schema: " + introducedSchema,
             "She said: 'Follow this JSON Schema: " + introducedSchema + "'. Explain.",
             "Summarize this example:\n```\nUse this JSON Schema: " + introducedSchema + "\n```",
             "Follow this JSON Schema: " + introducedSchema + ". Actually respond in prose."})
        Check(RequestedReplyContract(input).rootKind == ReplyFormat::Conversation,
            "A quoted, explanatory, withdrawn or negated schema introduction forced structured output.");
    for (const std::string separator :
        {" Ignore that shape.", " Do not return JSON.", " Change the task.", " Return a JSON array.", " Actually,"})
    {
        const auto isolated = RequestedReplyContract("Use this JSON Schema: " + introducedSchema + "." + separator +
                                                     R"( Return JSON with exactly keys "thermalRatio", "enabledFlag".)");
        Check(isolated.extractionStatus == ReplyContractStatus::ExplicitShape &&
                  nlohmann::json::parse(isolated.schemaJson).at("properties").at("thermalRatio").empty(),
            "An intervening clause resurrected superseded schema types.");
    }
    const auto changedKeys = RequestedReplyContract(
        "Follow this JSON Schema: " + introducedSchema + R"(. Return JSON with exactly keys "differentField". Return only valid JSON.)");
    Check(changedKeys.extractionStatus == ReplyContractStatus::ExplicitShape &&
              MatchesReplyContract(R"({"differentField":[]})", changedKeys) &&
              !MatchesReplyContract(R"({"thermalRatio":2.75,"enabledFlag":true})", changedKeys),
        "A different latest key directive retained obsolete schema fields.");
    const auto latestSchema = RequestedReplyContract(
        "Follow this JSON Schema: " + introducedSchema +
        R"(. Use this JSON Schema: {"type":"object","properties":{"newReading":{"type":"string"}},"required":["newReading"],"additionalProperties":false}. Return only valid JSON.)");
    Check(latestSchema.extractionStatus == ReplyContractStatus::ExplicitShape &&
              MatchesReplyContract(R"({"newReading":"literal"})", latestSchema) &&
              !MatchesReplyContract(R"({"thermalRatio":2.75,"enabledFlag":true})", latestSchema),
        "A latest explicit schema replacement lost authority.");
    for (const std::string earlier :
        {R"({"type":"object","properties":{"thermalRatio":{"type":"number"},"enabledFlag":{"type":"boolean"}},"required":["thermalRatio"],"additionalProperties":false})",
            R"({"type":"object","properties":{"thermalRatio":{"type":"number"},"enabledFlag":{"type":"boolean"}},"required":["thermalRatio","enabledFlag"],"additionalProperties":true})"})
    {
        const auto incompatible =
            RequestedReplyContract("Use this JSON Schema: " + earlier +
                                   R"(. Return JSON with exactly keys "thermalRatio", "enabledFlag". Return only valid JSON.)");
        Check(incompatible.extractionStatus == ReplyContractStatus::ExplicitShape &&
                  nlohmann::json::parse(incompatible.schemaJson).at("properties").at("thermalRatio").empty(),
            "An optional or open earlier schema was treated as an identical closed key reaffirmation.");
    }
    for (const std::string suffix : {". Unknown instruction. Return only valid JSON.", ". Actually, return only valid JSON.",
             ". Do not return JSON. Return only valid JSON.", ". Return a JSON array. Return only valid JSON.",
             ". \"Return only valid JSON.\""})
        Check(RequestedReplyContract("Follow this JSON Schema: " + introducedSchema + suffix).extractionStatus !=
                  ReplyContractStatus::ExplicitShape,
            "A terminal delivery resurrected a schema through an incompatible or masked barrier.");
    for (const std::string badSchema : {R"({"type":"object","type":"object"})", R"({"type":"object","$ref":"remote"})",
             R"({"type":"string"})", R"({"type":"object","properties":{"x":{"type":"string","pattern":".*"}}})", "{"})
    {
        const auto unsupported = RequestedReplyContract("Follow this JSON Schema: " + badSchema);
        Check(unsupported.rootKind == ReplyFormat::JsonObject && unsupported.extractionStatus == ReplyContractStatus::Unsupported &&
                  !unsupported.reason.empty() && MatchesReplyContract("{}", unsupported),
            "A malformed, unsupported or scalar schema introduction escaped conservative type-only handling.");
    }
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
    for (const std::string input :
        {R"(Return JSON with exactly keys "measurementValue". Return only the JSON object, without prose or a code fence.)",
            R"(Return JSON with exactly keys "measurementValue". Return only JSON.)",
            R"(Return JSON with exactly keys "measurementValue". Return JSON. Return only JSON object.)"})
    {
        const auto reaffirmed = RequestedReplyContract(input);
        Check(reaffirmed.extractionStatus == ReplyContractStatus::ExplicitShape &&
                  MatchesReplyContract(R"({"measurementValue":3.75})", reaffirmed) &&
                  !MatchesReplyContract(R"({"measurementvalue":3.75})", reaffirmed) && !MatchesReplyContract("{}", reaffirmed),
            "Compatible terminal JSON delivery lost explicit literal keys: " + input);
    }
    const auto reaffirmedSchema = RequestedReplyContract(
        R"(Return JSON using this schema: {"type":"object","properties":{"sampleRatio":{"type":"number"}},"required":["sampleRatio"],"additionalProperties":false}. Return only JSON.)");
    Check(reaffirmedSchema.extractionStatus == ReplyContractStatus::ExplicitShape &&
              nlohmann::json::parse(reaffirmedSchema.schemaJson).at("properties").at("sampleRatio").at("type") == "number" &&
              MatchesReplyContract(R"({"sampleRatio":1.25})", reaffirmedSchema) &&
              !MatchesReplyContract(R"({"sampleRatio":"1.25"})", reaffirmedSchema),
        "Compatible terminal JSON delivery lost or strengthened an explicit number schema.");
    for (const std::string input :
        {R"(Return JSON with keys oldField. Ignore that shape. Return JSON.)", R"(Return JSON with keys oldField. Actually, return JSON.)",
            R"(Return JSON with keys oldField. Do not return JSON. Return JSON.)",
            R"(Return JSON with keys oldField. Return a JSON array. Return JSON.)",
            R"(Return JSON with keys oldField. Change the requested task. Return only the JSON object.)",
            R"(Return JSON with keys oldField. "Return only JSON.")", "Return JSON with keys oldField.\n```\nReturn only JSON.\n```"})
        Check(RequestedReplyContract(input).extractionStatus != ReplyContractStatus::ExplicitShape,
            "Unknown, superseded or quoted intervening text resurrected an old shape: " + input);
    const auto replacementShape =
        RequestedReplyContract("Return JSON with keys oldField. Return JSON with keys newField. Return only JSON.");
    Check(replacementShape.extractionStatus == ReplyContractStatus::ExplicitShape &&
              MatchesReplyContract(R"({"newField":false})", replacementShape) &&
              !MatchesReplyContract(R"({"oldField":false})", replacementShape),
        "Compatible delivery resurrected a superseded explicit shape.");
    std::string repeatedDelivery = "Return JSON with keys boundedField.";
    for (int index = 0; index < 17; ++index)
        repeatedDelivery += " Return only JSON.";
    const auto capped = RequestedReplyContract(repeatedDelivery);
    Check(capped.extractionStatus == ReplyContractStatus::TypeOnly && capped.schemaJson == R"({"type":"object"})" &&
              MatchesReplyContract("{}", capped),
        "Delivery reaffirmations beyond the bounded scan resurrected an earlier shape.");
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
