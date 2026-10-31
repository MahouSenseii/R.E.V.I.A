#include "Audit/contentDigest.h"
#include "Learning/datasetExport.h"
#include "testSupport.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace
{
using namespace revia::learning;
using nlohmann::json;
using revia::tests::Check;

const DatasetExportOptions Options{"companion-behavior", "fixture-salt", 10, 10};

DatasetCandidate Candidate(const std::string& id, const std::string& session = "session-1")
{
    DatasetCandidate value;
    value.exampleId = id;
    value.origin = {"revia", session, 1, "", "", 1};
    value.sourceIds = {"attachment-" + id};
    value.prompt = {{"system", "You are Revia."}, {"user", "How did the poem about " + id + " make you feel?"}};
    value.response = "Honestly, the last line about " + id + " made me smile.";
    value.rejectedResponse = "The image shows a lake at dusk.";
    value.targetOrigin = DatasetTargetOrigin::ReviewedCorrection;
    auto& judgment = value.judgment;
    judgment.origin = value.origin;
    judgment.audienceRevision = 1;
    judgment.judgmentId = "judgment-" + id;
    judgment.sourceId = "turn-in-" + id;
    judgment.targetId = "turn-out-" + id;
    judgment.targetDigest = revia::audit::ContentDigest(value.rejectedResponse);
    judgment.criterion = "Answers the personal question that was asked.";
    judgment.evidence = "The owner judged the reply a repeated image description.";
    judgment.dependencies = {{"loaded-profile-sha256", revia::audit::ContentDigest("profile")},
        {"effective-llm-config-sha256", revia::audit::ContentDigest("configuration")}, {"native-answer-contract", "judged-quality-v1"}};
    value.review = LearningDecision::Accept;
    value.modelId = "Qwen3.5-9B-Q4_K_M.gguf";
    value.templateId = "qwen35-jinja";
    value.transformationVersion = "capture-v1";
    return value;
}

DatasetExport Build(const std::vector<DatasetCandidate>& candidates, const std::set<std::string>& withdrawn = {},
    const std::string& prior = {})
{
    DatasetExport result;
    std::string error;
    Check(BuildDatasetExport(candidates, withdrawn, Options, prior, result, error), "A valid dataset export failed: " + error);
    return result;
}

DatasetDecision DecisionFor(const DatasetExport& value, const std::string& id)
{
    const auto found = std::find_if(
        value.decisions.begin(), value.decisions.end(), [&id](const DatasetDecision& decision) { return decision.exampleId == id; });
    Check(found != value.decisions.end(), "No decision was recorded for " + id);
    return *found;
}

std::string AllExamples(const DatasetExport& value)
{
    return value.files.at("train.jsonl") + value.files.at("validation.jsonl") + value.files.at("test.jsonl");
}

void TestReviewedCorrectionIsTheOnlyTarget()
{
    const auto exported = Build({Candidate("a")});
    const auto& decision = DecisionFor(exported, "a");
    Check(decision.eligible, "An accepted, exactly bound correction was excluded: " + decision.reason);
    const auto line = exported.files.at(std::string(ToString(decision.partition)) + ".jsonl");
    const auto record = json::parse(line.substr(0, line.find('\n')));
    Check(record["messages"].back() == json{{"role", "assistant"}, {"content", Candidate("a").response}},
        "The reviewed correction was not the assistant target.");
    Check(record["rejected"] == Candidate("a").rejectedResponse && record["lineage"]["judgment"] == "judgment-a" &&
              record["lineage"]["origin"] == "reviewed correction",
        "The failed answer or the receipt lineage was not kept with the correction.");
    for (const auto& message : record["messages"])
        Check(message["content"] != Candidate("a").rejectedResponse, "A failed answer became a supervised message.");
}

void TestUnreviewedStaleOrUnsafeMaterialIsRejected()
{
    struct Case
    {
        std::string reason;
        DatasetCandidate candidate;
    };
    std::vector<Case> cases;
    const auto add = [&cases](const std::string& reason, const std::string& id, const auto& change)
    {
        auto value = Candidate(id);
        change(value);
        cases.push_back({reason, value});
    };
    add("review has not accepted", "pending", [](DatasetCandidate& value) { value.review = LearningDecision::Pending; });
    add("review has not accepted", "rework", [](DatasetCandidate& value) { value.review = LearningDecision::Rework; });
    add("repeats the failed answer", "same", [](DatasetCandidate& value) { value.response = value.rejectedResponse; });
    add("exact judged failure", "stale", [](DatasetCandidate& value) { value.rejectedResponse = "A different later reply."; });
    add("recorded criterion accepted", "unverified",
        [](DatasetCandidate& value)
        {
            value.targetOrigin = DatasetTargetOrigin::VerifiedOutput;
            value.rejectedResponse.clear();
        });
    add("recorded criterion accepted", "changed",
        [](DatasetCandidate& value)
        {
            value.targetOrigin = DatasetTargetOrigin::VerifiedOutput;
            value.rejectedResponse.clear();
            value.judgment.criterionSatisfied = true;
        });
    add("provenance is missing", "unpinned", [](DatasetCandidate& value) { value.judgment.dependencies.erase("effective-llm-config-sha256"); });
    add("provenance is missing", "untemplated", [](DatasetCandidate& value) { value.templateId.clear(); });
    add("another session", "foreign", [](DatasetCandidate& value) { value.judgment.origin.sessionId = "session-2"; });
    add("receipt is incomplete", "nodigest", [](DatasetCandidate& value) { value.judgment.targetDigest = "not-a-digest"; });
    add("turn being answered", "ended", [](DatasetCandidate& value) { value.prompt.push_back({"assistant", "Already answered."}); });
    add("GitHub personal access token", "secret",
        [](DatasetCandidate& value) { value.response += " My token is ghp_A1b2C3d4E5f6G7h8I9j0K1l2M3n4O5p6Q7r8."; });
    add("withdrawn", "withdrawn", [](DatasetCandidate& value) { value.sourceIds.push_back("forgotten-image"); });
    add("withdrawn", "forgotten-turn", [](DatasetCandidate& value) { value.judgment.targetId = "forgotten-image"; });
    auto duplicate = Candidate("original");
    duplicate.exampleId = "zz-copy";
    duplicate.judgment.judgmentId = "judgment-zz-copy";
    cases.push_back({"duplicates original", duplicate});

    std::vector<DatasetCandidate> candidates = {Candidate("original")};
    for (const auto& item : cases)
        candidates.push_back(item.candidate);
    const auto exported = Build(candidates, {"forgotten-image"});
    Check(DecisionFor(exported, "original").eligible, "The control example was excluded.");
    for (const auto& item : cases)
    {
        const auto& decision = DecisionFor(exported, item.candidate.exampleId);
        Check(!decision.eligible && decision.reason.find(item.reason) != std::string::npos,
            item.candidate.exampleId + " was not excluded for '" + item.reason + "': " + decision.reason);
        Check(AllExamples(exported).find("\"" + item.candidate.exampleId + "\"") == std::string::npos ||
                  item.candidate.exampleId == "zz-copy",
            item.candidate.exampleId + " reached an exported partition.");
    }
    Check(AllExamples(exported).find("zz-copy") == std::string::npos, "A duplicate example was exported twice.");
    const auto manifest = json::parse(exported.files.at("manifest.json"));
    Check(manifest["examples"].size() == candidates.size(), "The manifest did not record every decision, including exclusions.");
}

void TestExportIsDeterministic()
{
    std::vector<DatasetCandidate> candidates;
    for (int index = 0; index < 40; ++index)
        candidates.push_back(Candidate("example-" + std::to_string(index), "session-" + std::to_string(index % 7)));
    const auto first = Build(candidates);
    std::reverse(candidates.begin(), candidates.end());
    const auto reversed = Build(candidates);
    Check(first.files == reversed.files, "The same candidates in another order produced different bytes.");
    const auto manifest = json::parse(first.files.at("manifest.json"));
    for (const auto& [name, file] : manifest["files"].items())
        Check(file["sha256"] == revia::audit::ContentDigest(first.files.at(name)), "The manifest digest does not match " + name);
}

void TestRelatedExamplesShareAPartition()
{
    std::vector<DatasetCandidate> candidates;
    for (int index = 0; index < 200; ++index)
        candidates.push_back(Candidate("solo-" + std::to_string(index), "solo-session-" + std::to_string(index)));
    auto sharedSourceA = Candidate("shared-source-a", "left");
    auto sharedSourceB = Candidate("shared-source-b", "right");
    sharedSourceA.sourceIds.push_back("same-image");
    sharedSourceB.sourceIds.push_back("same-image");
    auto samePromptA = Candidate("same-prompt-a", "up");
    auto samePromptB = Candidate("same-prompt-b", "down");
    samePromptB.prompt = samePromptA.prompt;
    auto sameTaskA = Candidate("same-task-a", "north");
    auto sameTaskB = Candidate("same-task-b", "south");
    sameTaskA.origin.taskId = sameTaskB.origin.taskId = "task-7";
    sameTaskA.judgment.origin = sameTaskA.origin;
    sameTaskB.judgment.origin = sameTaskB.origin;
    for (const auto& value : {sharedSourceA, sharedSourceB, samePromptA, samePromptB, sameTaskA, sameTaskB})
        candidates.push_back(value);
    const auto exported = Build(candidates);
    for (const auto& [first, second] : {std::pair{"shared-source-a", "shared-source-b"}, std::pair{"same-prompt-a", "same-prompt-b"},
             std::pair{"same-task-a", "same-task-b"}})
    {
        const auto& left = DecisionFor(exported, first);
        const auto& right = DecisionFor(exported, second);
        Check(left.eligible && right.eligible && left.groupId == right.groupId && left.partition == right.partition,
            std::string(first) + " and " + second + " were split across partitions.");
    }
    std::set<DatasetPartition> used;
    for (const auto& decision : exported.decisions)
        used.insert(decision.partition);
    Check(used.size() == 3, "Two hundred independent sessions did not reach all three partitions.");
}

void TestEarlierPartitionsAreKept()
{
    std::vector<DatasetCandidate> candidates;
    for (int index = 0; index < 80; ++index)
        candidates.push_back(Candidate("kept-" + std::to_string(index), "kept-session-" + std::to_string(index)));
    const auto first = Build(candidates);
    const DatasetDecision* train = nullptr;
    const DatasetDecision* test = nullptr;
    for (const auto& decision : first.decisions)
    {
        if (!train && decision.partition == DatasetPartition::Train)
            train = &decision;
        if (!test && decision.partition == DatasetPartition::Test)
            test = &decision;
    }
    Check(train && test, "The fixture did not produce both a train and a test example.");

    // A new example joins a train example's group with a test example's group.
    auto bridge = Candidate("bridge", "bridge-session");
    bridge.sourceIds = {"attachment-" + train->exampleId, "attachment-" + test->exampleId};
    auto next = candidates;
    next.push_back(bridge);
    const auto second = Build(next, {}, first.files.at("manifest.json"));
    Check(DecisionFor(second, test->exampleId).eligible && DecisionFor(second, test->exampleId).partition == DatasetPartition::Test &&
              DecisionFor(second, "bridge").partition == DatasetPartition::Test,
        "A held-out test group did not stay in test when a related example arrived.");
    const auto& moved = DecisionFor(second, train->exampleId);
    Check(!moved.eligible && moved.reason.find("spans partitions") != std::string::npos,
        "An example that trained a model was moved into the test partition.");
    for (const auto& decision : first.decisions)
        if (decision.exampleId != train->exampleId && decision.exampleId != test->exampleId)
            Check(DecisionFor(second, decision.exampleId).partition == decision.partition,
                decision.exampleId + " changed partition between exports.");

    DatasetExport rejected;
    std::string error;
    Check(!BuildDatasetExport(next, {}, Options, "{not json", rejected, error), "An unreadable earlier manifest was ignored.");
}

void TestWithdrawalIsRecheckedBeforeUse()
{
    const auto exported = Build({Candidate("a"), Candidate("b", "session-2")});
    const auto& manifest = exported.files.at("manifest.json");
    std::vector<std::string> stale;
    std::string error;
    Check(StaleDatasetExamples(manifest, {}, stale, error) && stale.empty(), "An unchanged export was reported stale.");
    Check(StaleDatasetExamples(manifest, {"attachment-b"}, stale, error) && stale == std::vector<std::string>{"b"},
        "A source withdrawn after export was not reported before use.");
    Check(StaleDatasetExamples(manifest, {"turn-out-a"}, stale, error) && stale == std::vector<std::string>{"a"},
        "A forgotten judged turn was not reported before use.");
    Check(!StaleDatasetExamples("[]", {}, stale, error), "A manifest without the dataset schema was accepted.");
}

// The interchange shape a capture tool writes.
json CandidateFileEntry(const DatasetCandidate& value)
{
    const auto stamp = [](const revia::runtime::RuntimeStamp& origin)
    {
        return json{{"companionId", origin.companionId}, {"sessionId", origin.sessionId}, {"generation", origin.generation},
            {"taskId", origin.taskId}, {"attemptId", origin.attemptId}, {"policyVersion", origin.policyVersion}};
    };
    json prompt = json::array();
    for (const auto& message : value.prompt)
        prompt.push_back({{"role", message.role}, {"content", message.content}});
    const auto& feedback = value.judgment;
    return {{"exampleId", value.exampleId}, {"origin", stamp(value.origin)}, {"sourceIds", value.sourceIds}, {"prompt", prompt},
        {"response", value.response}, {"targetOrigin", "reviewed-correction"}, {"rejectedResponse", value.rejectedResponse},
        {"judgment", {{"source", "owner-judgment"}, {"target", "conversation-turn"}, {"issue", "answer-coverage"},
                         {"judgmentId", feedback.judgmentId}, {"sourceId", feedback.sourceId}, {"targetId", feedback.targetId},
                         {"targetDigest", feedback.targetDigest}, {"criterion", feedback.criterion}, {"evidence", feedback.evidence},
                         {"dependencies", feedback.dependencies}, {"criterionSatisfied", false}, {"privateContextExcluded", false},
                         {"audienceRevision", feedback.audienceRevision}, {"origin", stamp(feedback.origin)}}},
        {"review", "accept"}, {"modelId", value.modelId}, {"templateId", value.templateId},
        {"transformationVersion", value.transformationVersion}};
}

void TestCandidateFileIsReadStrictly()
{
    const auto entry = CandidateFileEntry(Candidate("a"));
    const auto file = [](const json& item) { return json{{"schema", "revia-dataset-candidates-v1"}, {"candidates", {item}}}.dump(); };
    std::vector<DatasetCandidate> read;
    std::string error;
    Check(ReadDatasetCandidates(file(entry), read, error) && read.size() == 1, "A well-formed candidate file was refused: " + error);
    Check(read[0].prompt.size() == 2 && read[0].response == Candidate("a").response &&
              read[0].judgment.targetDigest == Candidate("a").judgment.targetDigest && read[0].review == LearningDecision::Accept &&
              read[0].targetOrigin == DatasetTargetOrigin::ReviewedCorrection,
        "The candidate file did not round-trip its fields.");
    Check(DecisionFor(Build(read), "a").eligible, "A candidate read from the interchange file was not eligible.");

    std::vector<json> broken;
    auto unknownReview = entry;
    unknownReview["review"] = "maybe";
    broken.push_back(unknownReview);
    auto noCriterion = entry;
    noCriterion["judgment"].erase("criterion");
    broken.push_back(noCriterion);
    auto image = entry;
    image["prompt"][1]["content"] = json::array({json{{"type", "image_url"}}});
    broken.push_back(image);
    auto unknownIssue = entry;
    unknownIssue["judgment"]["issue"] = "vibes";
    broken.push_back(unknownIssue);
    for (const auto& item : broken)
        Check(!ReadDatasetCandidates(file(item), read, error) && read.empty(), "A malformed candidate was accepted: " + item.dump());
    Check(!ReadDatasetCandidates(json{{"schema", "other"}, {"candidates", json::array()}}.dump(), read, error),
        "A candidate file with another schema was accepted.");
}

void TestOfferedToolsTravelWithTheExample()
{
    // The definitions the request offered are part of what the template rendered.
    const json tools = json::parse(R"([{"type": "function", "function": {"name": "calculator",
        "description": "Evaluate one arithmetic expression.",
        "parameters": {"type": "object", "properties": {"expression": {"type": "string"}}}}}])");
    auto offered = Candidate("a");
    offered.tools = tools.dump();
    const auto exported = Build({offered, Candidate("b")});
    const auto& decision = DecisionFor(exported, "a");
    Check(decision.eligible, "An example that offered tools was excluded: " + decision.reason);
    bool sawTools = false;
    bool sawPlain = false;
    const auto lines = AllExamples(exported);
    for (std::size_t begin = 0, end; (end = lines.find('\n', begin)) != std::string::npos; begin = end + 1)
    {
        const auto record = json::parse(lines.substr(begin, end - begin));
        if (record["id"] == "a")
            sawTools = record.contains("tools") && record["tools"] == tools;
        if (record["id"] == "b")
            sawPlain = !record.contains("tools");
    }
    Check(sawTools && sawPlain, "The offered tools were not exported exactly, or were invented for a request without them.");
    auto same = Candidate("c");
    same.prompt = offered.prompt;
    same.response = offered.response;
    Check(DecisionFor(Build({offered, same}), "c").eligible, "The same prompt without tools was counted as a duplicate.");

    std::vector<std::string> malformed = {"{}", "[]", R"([{"type":"function"}])", R"([{"type":"other","function":{"name":"x"}}])",
        R"([{"type":"function","function":{"name":""}}])", R"([{"type":"function","function":{"name":"x","parameters":"s"}}])", "[not json"};
    for (const auto& text : malformed)
    {
        auto broken = Candidate("a");
        broken.tools = text;
        Check(!DecisionFor(Build({broken}), "a").eligible, "Malformed tool definitions were exported: " + text);
    }

    auto entry = CandidateFileEntry(offered);
    entry["tools"] = tools;
    std::vector<DatasetCandidate> read;
    std::string error;
    const auto file = [](const json& item) { return json{{"schema", "revia-dataset-candidates-v1"}, {"candidates", {item}}}.dump(); };
    Check(ReadDatasetCandidates(file(entry), read, error) && read.size() == 1 && json::parse(read[0].tools) == tools,
        "The candidate file did not carry the offered tools: " + error);
    entry["tools"] = json::array();
    Check(ReadDatasetCandidates(file(entry), read, error) && read[0].tools.empty(), "An empty tool list was not read as none.");
    entry["tools"] = "calculator";
    Check(!ReadDatasetCandidates(file(entry), read, error), "Tools that are not an array were accepted.");
}

void TestConversationsGroupInsteadOfTheirSession()
{
    // Forty chats in one runtime session, as an owner judges replies in one run of the app.
    std::vector<DatasetCandidate> chats;
    for (int number = 0; number < 40; ++number)
    {
        auto value = Candidate("chat-" + std::to_string(number));
        value.sourceIds = {"conversation:chat-" + std::to_string(number)};
        chats.push_back(value);
    }
    auto sameChat = Candidate("chat-0-later");
    sameChat.sourceIds = {"conversation:chat-0"};
    chats.push_back(sameChat);
    auto unnamedA = Candidate("unnamed-a");
    auto unnamedB = Candidate("unnamed-b");
    chats.push_back(unnamedA);
    chats.push_back(unnamedB);
    const auto exported = Build(chats);
    std::set<std::string> groups;
    std::set<DatasetPartition> partitions;
    for (int number = 0; number < 40; ++number)
    {
        const auto decision = DecisionFor(exported, "chat-" + std::to_string(number));
        groups.insert(decision.groupId);
        partitions.insert(decision.partition);
    }
    Check(groups.size() == 40 && partitions.size() > 1, "Chats from one runtime session were kept in one partition.");
    Check(DecisionFor(exported, "chat-0-later").groupId == DecisionFor(exported, "chat-0").groupId,
        "Two turns of the same chat were split into different groups.");
    Check(DecisionFor(exported, "unnamed-a").groupId == DecisionFor(exported, "unnamed-b").groupId,
        "Candidates that name no conversation no longer group by their session.");
}

void TestExportsAreNeverReplaced()
{
    revia::tests::ScopedTestDirectory directory;
    const auto exported = Build({Candidate("a")});
    const auto target = directory.root / "dataset-1";
    std::string error;
    Check(WriteDatasetExport(exported, target, error), "A new dataset export could not be written: " + error);
    for (const auto& [name, bytes] : exported.files)
    {
        std::ifstream stream(target / name, std::ios::binary);
        const std::string written{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        Check(written == bytes, name + " was not written exactly.");
    }
    Check(!WriteDatasetExport(exported, target, error), "An existing dataset export was replaced.");
    std::filesystem::create_directories(directory.root / "dataset-2.partial");
    Check(!WriteDatasetExport(exported, directory.root / "dataset-2", error), "An unfinished export beside the target was ignored.");
    auto escaping = exported;
    escaping.files["../outside.jsonl"] = "{}\n";
    Check(!WriteDatasetExport(escaping, directory.root / "dataset-3", error) &&
              !std::filesystem::exists(directory.root / "outside.jsonl"),
        "A file name outside the export directory was written.");
}
}

void RunDatasetExportTests()
{
    std::string errors;
    for (const auto test : {TestReviewedCorrectionIsTheOnlyTarget, TestUnreviewedStaleOrUnsafeMaterialIsRejected, TestExportIsDeterministic,
             TestRelatedExamplesShareAPartition, TestEarlierPartitionsAreKept, TestWithdrawalIsRecheckedBeforeUse,
             TestCandidateFileIsReadStrictly, TestOfferedToolsTravelWithTheExample, TestConversationsGroupInsteadOfTheirSession,
             TestExportsAreNeverReplaced})
        try
        {
            test();
        }
        catch (const std::exception& error)
        {
            errors += std::string(error.what()) + '\n';
        }
    Check(errors.empty(), errors);
    std::cout << "Dataset eligibility, grouping, partitions, withdrawal and immutable export passed.\n";
}
