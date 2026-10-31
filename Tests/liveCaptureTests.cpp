#include "Audit/contentDigest.h"
#include "Identity/promptMarkers.h"
#include "Learning/datasetExport.h"
#include "Learning/liveCapture.h"
#include "testSupport.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{
using namespace revia::learning;
using nlohmann::json;
using revia::tests::Check;

constexpr const char* Displayed = "The image shows a lake at dusk.";

// The shape of a conversation request llamaCppService sends: sorted keys, thinking off,
// DRY on and the calculator offered, as on any private reply.
json Request()
{
    return {{"model", "Qwen3.5-9B-Q4_K_M.gguf"},
        {"messages", {{{"role", "system"}, {"content", "You are Revia."}}, {{"role", "user"}, {"content", "Hi there."}},
                         {{"role", "assistant"}, {"content", "Hello!"}},
                         {{"role", "user"}, {"content", "How did the poem make you feel?"}}}},
        {"tools", json::parse(R"([{"type": "function", "function": {"name": "calculator", "description": "Evaluate arithmetic.",
            "parameters": {"type": "object", "properties": {"expression": {"type": "string"}}, "required": ["expression"]}}}])")},
        {"tool_choice", "auto"}, {"parallel_tool_calls", true}, {"temperature", 0.7}, {"max_tokens", 1024}, {"stream", true},
        {"cache_prompt", true}, {"chat_template_kwargs", {{"enable_thinking", false}}}, {"reasoning_budget_tokens", 0},
        {"dry_multiplier", 0.8}, {"dry_base", 1.75}, {"dry_allowed_length", 2}, {"dry_penalty_last_n", 4096},
        {"stop", json::array({"<|im_end|>"})}};
}

DeliveredReply Reply(const json& request = Request())
{
    DeliveredReply value;
    value.origin = {"revia", "session-1", 3, "", "", 2};
    value.audienceRevision = 4;
    value.conversationId = "conversation-1";
    value.participantId = "owner";
    value.displayed = Displayed;
    value.modelText = Displayed;
    value.profileDigest = revia::audit::ContentDigest("profile");
    value.configurationDigest = revia::audit::ContentDigest("configuration");
    auto exchange = std::make_shared<revia::llm::ProviderExchange>();
    exchange->requestBody = request.dump();
    exchange->responseText = Displayed;
    exchange->finishReason = "stop";
    exchange->completeStream = true;
    exchange->model = "Qwen3.5-9B-Q4_K_M.gguf";
    exchange->templateSha256 = revia::audit::ContentDigest("{{ messages }}");
    value.exchange = std::move(exchange);
    return value;
}

QualityFeedback Judgment(const DeliveredReply& reply = Reply())
{
    QualityFeedback value;
    value.origin = reply.origin;
    value.audienceRevision = reply.audienceRevision;
    value.issue = QualityIssue::AnswerCoverage;
    value.judgmentId = "judgment-1";
    value.sourceId = "owner-review:judgment-1";
    value.targetId = "displayed-reply:reply-1";
    value.targetDigest = revia::audit::ContentDigest(reply.displayed);
    value.criterion = "Answers the personal question that was asked.";
    value.evidence = "The owner judged the reply a repeated image description.";
    value.dependencies = {{"displayed-result", value.targetDigest}, {"answer-contract", "authored-answer-obligation-v1"},
        {"loaded-profile-sha256", reply.profileDigest}, {"effective-llm-config-sha256", reply.configurationDigest},
        {"native-answer-contract", "judged-quality-v1"}};
    return value;
}

// Another judged reply: the same request, as when a reply is regenerated, but its own text and
// its own displayed-reply ID.
std::pair<DeliveredReply, QualityFeedback> OtherReply(const std::string& conversationId = "conversation-1")
{
    auto reply = Reply();
    reply.conversationId = conversationId;
    reply.displayed = reply.modelText = "I can't say.";
    auto judgment = Judgment(reply);
    judgment.targetId = "displayed-reply:reply-2";
    return {reply, judgment};
}

LiveCapture Built(const DeliveredReply& reply = Reply(), const QualityFeedback& judgment = Judgment())
{
    LiveCapture capture;
    std::string error;
    Check(BuildLiveCapture(reply, judgment, capture, error), "A judged, exactly held reply was refused: " + error);
    return capture;
}

std::string Refusal(const DeliveredReply& reply, const QualityFeedback& judgment)
{
    LiveCapture capture;
    std::string error;
    Check(!BuildLiveCapture(reply, judgment, capture, error) && capture.record.empty() && !error.empty(),
        "An inexact or unwithdrawable exchange was kept.");
    return error;
}

void Expect(const std::string& reason, const std::string& expected)
{
    Check(reason.find(expected) != std::string::npos, "Refused for \"" + reason + "\" instead of \"" + expected + "\".");
}

void TestJudgedReplyKeepsItsExactExchange()
{
    const auto capture = Built();
    const auto record = json::parse(capture.record);
    const auto request = Request();
    Check(record["prompt"] == request["messages"] && record["tools"] == request["tools"],
        "The capture did not keep the exact messages and tools the model received.");
    Check(record["failedOutput"] == Displayed && record["failedOutputSha256"] == revia::audit::ContentDigest(Displayed) &&
              record["judgment"]["targetDigest"] == record["failedOutputSha256"] && record["repaired"] == false,
        "The judged reply and its digest were not kept together.");
    const auto requestDigest = revia::audit::ContentDigest(request.dump());
    Check(capture.sources == std::vector<std::string>{"request:" + requestDigest, "conversation:conversation-1", "participant:owner"} &&
              record["sources"] == capture.sources && record["source"]["requestBodySha256"] == requestDigest,
        "The capture does not name the request, conversation and speaker that withdraw it.");
    Check(record["requestSettings"]["dry_multiplier"] == 0.8 && record["requestSettings"]["tool_choice"] == "auto" &&
              !record["requestSettings"].contains("cache_prompt") && record["provenance"]["templateSha256"] == Reply().exchange->templateSha256,
        "The capture did not keep the generation settings and provenance.");
    Check(Built().record == capture.record, "The same judged reply did not produce the same capture.");

    // The record reaches the exporter through the candidate interchange unchanged.
    json entry = {{"exampleId", "live:" + capture.captureId}, {"origin", record["origin"]}, {"sourceIds", record["sources"]},
        {"prompt", record["prompt"]}, {"tools", record["tools"]}, {"response", "Honestly, the last line made me smile."},
        {"targetOrigin", "reviewed-correction"}, {"rejectedResponse", record["failedOutput"]}, {"judgment", record["judgment"]},
        {"review", "accept"}, {"modelId", record["provenance"]["model"]},
        {"templateId", "chat-template-sha256:" + record["provenance"]["templateSha256"].get<std::string>()},
        {"transformationVersion", LiveCaptureVersion}};
    std::vector<DatasetCandidate> candidates;
    std::string error;
    Check(ReadDatasetCandidates(json{{"schema", "revia-dataset-candidates-v1"}, {"candidates", {entry}}}.dump(), candidates, error),
        "The interchange refused a live capture's judgment: " + error);
    DatasetExport exported;
    Check(BuildDatasetExport(candidates, {}, {"live", "salt", 10, 10}, {}, exported, error) && exported.decisions.size() == 1 &&
              exported.decisions[0].eligible,
        "A live capture with an accepted correction was not exportable: " +
            (exported.decisions.empty() ? error : exported.decisions[0].reason));
    Check(!BuildDatasetExport(candidates, {"conversation:conversation-1"}, {"live", "salt", 10, 10}, {}, exported, error) ||
              !exported.decisions[0].eligible,
        "Deleting the conversation did not withdraw its live capture.");

    // The contract fixture the Python capture tests read.
    if (const char* output = std::getenv("REVIA_LIVE_CAPTURE_FIXTURE_OUT"))
        std::ofstream(output, std::ios::binary) << capture.record;
    std::ifstream fixture("Tests/Fixture/liveCapture.json", std::ios::binary);
    const std::string expected{std::istreambuf_iterator<char>(fixture), std::istreambuf_iterator<char>()};
    Check(!expected.empty() && json::parse(expected) == record,
        "Tests/Fixture/liveCapture.json no longer matches the live capture; regenerate it with REVIA_LIVE_CAPTURE_FIXTURE_OUT.");
}

void TestRepairedReplyKeepsTheModelsOwnText()
{
    auto reply = Reply();
    reply.modelText = std::string(Displayed) + " " + Displayed;
    reply.displayed = Displayed;
    const auto record = json::parse(Built(reply, Judgment(reply)).record);
    Check(record["repaired"] == true && record["modelOutput"] == reply.exchange->responseText &&
              record["failedOutput"] == reply.displayed,
        "A reply changed by style repair lost either the judged text or the model's own.");
}

void TestInexactOrUnwithdrawableExchangesAreRefused()
{
    const auto reply = Reply();
    auto judgment = Judgment();
    judgment.criterionSatisfied = true;
    Expect(Refusal(reply, judgment), "failed can keep");
    judgment = Judgment();
    judgment.source = QualityEvidenceSource::DeterministicAcceptance;
    Expect(Refusal(reply, judgment), "failed can keep");
    judgment = Judgment();
    judgment.targetDigest = revia::audit::ContentDigest("Another reply");
    Expect(Refusal(reply, judgment), "not bound");
    judgment = Judgment();
    ++judgment.audienceRevision;
    Expect(Refusal(reply, judgment), "not bound");
    judgment = Judgment();
    ++judgment.origin.generation;
    Expect(Refusal(reply, judgment), "not bound");
    judgment = Judgment();
    judgment.dependencies["loaded-profile-sha256"] = revia::audit::ContentDigest("edited profile");
    Expect(Refusal(reply, judgment), "changed after this reply");

    auto changed = Reply();
    changed.replaced = true;
    Expect(Refusal(changed, Judgment(changed)), "replaced the model's answer");
    changed = Reply();
    changed.exchange.reset();
    Expect(Refusal(changed, Judgment(changed)), "No local model exchange");
    const auto withExchange = [](const std::function<void(revia::llm::ProviderExchange&)>& edit)
    {
        auto value = Reply();
        auto exchange = std::make_shared<revia::llm::ProviderExchange>(*value.exchange);
        edit(*exchange);
        value.exchange = std::move(exchange);
        return Refusal(value, Judgment(value));
    };
    Expect(withExchange([](auto& exchange) { exchange.templateSha256.clear(); }), "chat template is unknown");
    Expect(withExchange([](auto& exchange) { exchange.requestedTools = true; }), "tool calls");
    Expect(withExchange([](auto& exchange) { exchange.requestBody = "{not json"; }), "no messages");
    Expect(withExchange([](auto& exchange) { exchange.requestBody = R"({"messages": [{"role": 7, "content": "x"}]})"; }),
        "not well-formed");
    Expect(withExchange([](auto& exchange) { exchange.responseText = "password=synthetic-secret-credential"; }), "contains");

    const auto withRequest = [](const std::function<void(json&)>& edit)
    {
        auto request = Request();
        edit(request);
        const auto value = Reply(request);
        return Refusal(value, Judgment(value));
    };
    Expect(withRequest([](json& request)
               { request["messages"][3]["content"] = json::array({{{"type", "image_url"}, {"image_url", {{"url", "data:"}}}}}); }),
        "non-text parts");
    Expect(withRequest(
               [](json& request)
               {
                   request["messages"].push_back({{"role", "assistant"}, {"content", ""},
                       {"tool_calls", json::array({{{"id", "call_0"}, {"type", "function"}}})}});
                   request["messages"].push_back({{"role", "tool"}, {"content", "4"}});
               }),
        "tool-call history");
    Expect(withRequest([](json& request) { request["chat_template_kwargs"]["enable_thinking"] = true; }), "thinking off");
    Expect(withRequest([](json& request) { request.erase("chat_template_kwargs"); }), "thinking off");
    Expect(withRequest([](json& request) { request["tools"] = json::array({{{"type", "function"}}}); }), "named functions");
    Expect(withRequest(
               [](json& request)
               {
                   request["messages"][3]["content"] =
                       std::string(revia::identity::markers::RetrievedConversationBlock) + "\n  [yesterday] the user: hi\n\nHow?";
               }),
        "recalled from other chats");
    Expect(withRequest(
               [](json& request)
               {
                   request["messages"][0]["content"] =
                       "You are Revia.\n\n" + std::string(revia::identity::markers::AttachmentGrounding) + "\nnotes.txt: draft";
               }),
        "attached files");
    Expect(withRequest([](json& request) { request["messages"][3]["content"] = "My password=synthetic-secret-credential"; }),
        "contains");
}
std::string Read(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void TestKeptCapturesAreImmutableAndWithdrawn()
{
    revia::tests::ScopedTestDirectory directory;
    const LiveCaptureStore store(directory.root / "Captures");
    std::size_t removed = 0;
    std::string error;
    Check(store.Withdraw("conversation:conversation-1", removed, error) && removed == 0 &&
              !std::filesystem::exists(directory.root / "Captures"),
        "A withdrawal before any capture was kept created the capture store.");

    const auto first = Built();
    Check(store.Keep(first, error), "A new capture was not kept: " + error);
    Check(Read(store.LiveDirectory() / (first.captureId + ".json")) == first.record, "The kept capture is not the exact record.");
    Check(!store.Keep(first, error) && error.find("already kept") != std::string::npos, "A kept capture was replaced.");
    auto unnamed = first;
    unnamed.captureId = "../escape";
    Check(!store.Keep(unnamed, error), "A capture without its own identity was written.");

    const auto [otherReply, otherJudgment] = OtherReply("conversation-2");
    const auto second = Built(otherReply, otherJudgment);
    Check(store.Keep(second, error), error);
    Check(store.Withdraw("conversation:conversation-1", removed, error) && removed == 1, "Deleting a chat did not withdraw its capture: " + error);
    Check(!std::filesystem::exists(store.LiveDirectory() / (first.captureId + ".json")) &&
              std::filesystem::exists(store.LiveDirectory() / (second.captureId + ".json")),
        "The withdrawal did not remove exactly the deleted chat's capture.");
    auto listed = json::parse(Read(store.WithdrawnList())).get<std::vector<std::string>>();
    // Not the shared request: the other chat's capture stays eligible.
    Check(listed == std::vector<std::string>{"conversation:conversation-1", "displayed-reply:reply-1"},
        "The withdrawal list does not name exactly the chat and its capture's judged reply.");

    Check(store.WithdrawAll(removed, error) && removed == 1 && std::filesystem::is_empty(store.LiveDirectory()),
        "Forgetting every chat left a capture: " + error);
    listed = json::parse(Read(store.WithdrawnList())).get<std::vector<std::string>>();
    Check(listed == std::vector<std::string>{"conversation:conversation-1", "conversation:conversation-2", "displayed-reply:reply-1",
                        "displayed-reply:reply-2"},
        "Forgetting every chat did not list each capture's conversation and judged reply, or lost an earlier withdrawal.");

    Check(store.Keep(first, error), error);
    std::ofstream(store.WithdrawnList(), std::ios::binary | std::ios::trunc) << "{broken";
    Check(!store.Withdraw("participant:owner", removed, error) && removed == 0 &&
              std::filesystem::exists(store.LiveDirectory() / (first.captureId + ".json")),
        "An unreadable withdrawal list was replaced, or a capture deleted without being listed.");
}
void TestOwnerCorrectionsAreRevisedAndWithdrawn()
{
    revia::tests::ScopedTestDirectory directory;
    const LiveCaptureStore store(directory.root / "Captures");
    std::string error;
    Check(store.List(error).empty() && error.empty(), "An unused store listed captures.");
    const auto unused = store.Revision();
    const auto capture = Built();
    Check(store.Keep(capture, error), error);
    Check(store.Revision() != unused, "Keeping a capture did not change the store's revision.");
    auto listed = store.List(error);
    Check(listed.size() == 1 && listed[0].captureId == capture.captureId && listed[0].decision == LearningDecision::Pending &&
              listed[0].revision == 0 && listed[0].input == "How did the poem make you feel?" && listed[0].failedOutput == Displayed &&
              listed[0].criterion == Judgment().criterion && listed[0].issue == "answer-coverage" &&
              listed[0].conversationId == "conversation-1",
        "A kept capture was not listed with its judged reply, recorded judgment and pending decision.");

    std::uint64_t revision = 0;
    Check(!store.Correct(capture.captureId, LearningDecision::Accept, " \n", revision, error), "An accepted capture was saved without a reply.");
    Check(!store.Correct(capture.captureId, LearningDecision::Accept, Displayed, revision, error),
        "An accepted correction repeated the judged reply.");
    Check(!store.Correct(capture.captureId, LearningDecision::Pending, "Later.", revision, error), "Pending was saved as a decision.");
    Check(!store.Correct(capture.captureId, LearningDecision::Accept, "My password=synthetic-secret-credential", revision, error) &&
              error.find("contains") != std::string::npos,
        "A correction holding a secret was saved.");
    Check(!store.Correct("0123456789abcdef", LearningDecision::Reject, "", revision, error) &&
              !store.Correct("../escape", LearningDecision::Reject, "", revision, error),
        "A correction was saved for a capture that is not kept.");
    Check(!std::filesystem::exists(store.CorrectionsDirectory()), "A refused correction wrote something.");

    const std::string draft = "Draft: the last line made me smile.";
    const std::string correct = "Honestly, the last line about the lake made me smile.";
    Check(store.Correct(capture.captureId, LearningDecision::Rework, draft, revision, error) && revision == 1, error);
    Check(store.Correct(capture.captureId, LearningDecision::Accept, correct, revision, error) && revision == 2, error);
    listed = store.List(error);
    Check(listed.size() == 1 && listed[0].decision == LearningDecision::Accept && listed[0].correction == correct &&
              listed[0].revision == 2,
        "The latest revision is not the one listed.");
    const auto savedPath = store.CorrectionsDirectory() / (capture.captureId + ".json");
    const auto saved = json::parse(Read(savedPath));
    Check(saved["schema"] == "revia-live-correction-v1" && saved["decision"] == "accept" &&
              saved["failedOutputSha256"] == revia::audit::ContentDigest(Displayed) && saved["history"].size() == 1 &&
              saved["history"][0] == json{{"revision", 1}, {"decision", "rework"}, {"correction", draft}},
        "The earlier revision was not kept, or the correction is not bound to its capture.");
    // The contract fixture the Python capture tests read beside Tests/Fixture/liveCapture.json.
    if (const char* output = std::getenv("REVIA_LIVE_CORRECTION_FIXTURE_OUT"))
        std::ofstream(output, std::ios::binary) << Read(savedPath);
    const auto expected = Read("Tests/Fixture/liveCorrection.json");
    Check(!expected.empty() && json::parse(expected) == saved,
        "Tests/Fixture/liveCorrection.json no longer matches the saved correction; regenerate it with REVIA_LIVE_CORRECTION_FIXTURE_OUT.");

    // A regenerated reply from the same request, kept separately. A correction file that names
    // another reply is not shown as its decision.
    const auto [otherReply, otherJudgment] = OtherReply();
    const auto other = Built(otherReply, otherJudgment);
    Check(store.Keep(other, error), error);
    auto forged = saved;
    forged["captureId"] = other.captureId;
    std::ofstream(store.CorrectionsDirectory() / (other.captureId + ".json"), std::ios::binary) << forged.dump();
    listed = store.List(error);
    Check(listed.size() == 1 && listed[0].captureId == capture.captureId && error.find("1 kept capture") != std::string::npos,
        "A correction bound to another reply was listed as this capture's decision.");
    Check(!store.Correct(other.captureId, LearningDecision::Reject, "", revision, error) &&
              error.find("another capture") != std::string::npos,
        "A correction bound to another reply was revised.");

    const bool discarded = store.Discard(capture.captureId, error);
    Check(discarded, "A kept capture could not be deleted: " + error);
    Check(!std::filesystem::exists(store.LiveDirectory() / (capture.captureId + ".json")) && !std::filesystem::exists(savedPath) &&
              !std::filesystem::exists(savedPath.string() + ".bak") &&
              std::filesystem::exists(store.LiveDirectory() / (other.captureId + ".json")),
        "Deleting a capture left it or its correction behind, or deleted the capture sharing its request.");
    const auto withdrawn = json::parse(Read(store.WithdrawnList())).get<std::vector<std::string>>();
    Check(withdrawn == std::vector<std::string>{"displayed-reply:reply-1"},
        "Deleting one capture did not list exactly its judged reply as withdrawn.");
    Check(!store.Discard(capture.captureId, error), "A deleted capture was deleted again.");
    std::size_t removed = 0;
    Check(store.WithdrawAll(removed, error) && removed == 1 && std::filesystem::is_empty(store.CorrectionsDirectory()),
        "Forgetting every chat left a capture's correction behind: " + error);
}
} // namespace

void RunLiveCaptureTests()
{
    std::string errors;
    for (const auto test : {TestJudgedReplyKeepsItsExactExchange, TestRepairedReplyKeepsTheModelsOwnText,
             TestInexactOrUnwithdrawableExchangesAreRefused, TestKeptCapturesAreImmutableAndWithdrawn,
             TestOwnerCorrectionsAreRevisedAndWithdrawn})
        try
        {
            test();
        }
        catch (const std::exception& error)
        {
            errors += std::string(error.what()) + '\n';
        }
    Check(errors.empty(), errors);
    std::cout << "Live capture binding, exactness, refusals, immutable keeping, owner corrections and withdrawal passed.\n";
}
