#include "Audit/contentDigest.h"
#include "Learning/liveDatasetExport.h"
#include "testSupport.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{
using namespace revia::learning;
using nlohmann::json;
using revia::tests::Check;

std::string Read(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

// Points the OneDrive variable Windows sets at `folder`, or removes it.
void SetOneDrive(const std::filesystem::path& folder)
{
#ifdef _WIN32
    _wputenv_s(L"OneDrive", folder.c_str());
#else
    if (folder.empty())
        unsetenv("OneDrive");
    else
        setenv("OneDrive", folder.c_str(), 1);
#endif
}

void Write(const std::filesystem::path& path, const std::string& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
}

// One judged reply, by default from its own conversation. Each judgment of it shows the reply
// anew, so a later one has its own displayed-reply ID.
LiveCapture Judged(const int number, const std::string& judged = "", const int conversation = 0)
{
    const std::string question = "Question " + std::to_string(number) + "?";
    DeliveredReply reply;
    reply.origin = {"revia", "session-1", 1, "", "", 1};
    reply.audienceRevision = 1;
    reply.conversationId = "conversation-" + std::to_string(conversation == 0 ? number : conversation);
    reply.participantId = "owner";
    reply.displayed = reply.modelText = "Unhelpful reply " + std::to_string(number) + ".";
    reply.profileDigest = revia::audit::ContentDigest("profile");
    reply.configurationDigest = revia::audit::ContentDigest("configuration");
    auto exchange = std::make_shared<revia::llm::ProviderExchange>();
    exchange->requestBody = json{{"model", "main"},
        {"messages", {{{"role", "system"}, {"content", "You are Revia."}}, {{"role", "user"}, {"content", question}}}},
        {"chat_template_kwargs", {{"enable_thinking", false}}}, {"stream", true}}
                                .dump();
    exchange->responseText = reply.displayed;
    exchange->finishReason = "stop";
    exchange->model = "main";
    exchange->templateSha256 = revia::audit::ContentDigest("{{ messages }}");
    reply.exchange = std::move(exchange);
    QualityFeedback judgment;
    judgment.origin = reply.origin;
    judgment.audienceRevision = reply.audienceRevision;
    judgment.judgmentId = "judgment-" + std::to_string(number) + judged;
    judgment.sourceId = "owner-review:" + std::to_string(number) + judged;
    judgment.targetId = "displayed-reply:" + std::to_string(number) + judged;
    judgment.targetDigest = revia::audit::ContentDigest(reply.displayed);
    judgment.criterion = "Answers the question.";
    judgment.evidence = "It did not.";
    judgment.dependencies = {{"loaded-profile-sha256", reply.profileDigest}, {"effective-llm-config-sha256", reply.configurationDigest}};
    LiveCapture capture;
    std::string error;
    Check(BuildLiveCapture(reply, judgment, capture, error), "A synthetic capture was not built: " + error);
    return capture;
}

// Keeps a judged reply and, unless `decision` is Pending, saves its correction.
std::string Keep(const LiveCaptureStore& store, const int number, const LearningDecision decision = LearningDecision::Accept,
    const std::string& judged = "", const int conversation = 0)
{
    const auto capture = Judged(number, judged, conversation);
    std::string error;
    Check(store.Keep(capture, error), "A synthetic capture was not kept: " + error);
    std::uint64_t revision = 0;
    if (decision != LearningDecision::Pending)
        Check(store.Correct(capture.captureId, decision, "A helpful answer to question " + std::to_string(number) + ".", revision, error),
            "A synthetic correction was not saved: " + error);
    return capture.captureId;
}

std::map<std::string, std::string> Partitions(const std::filesystem::path& manifest)
{
    std::map<std::string, std::string> placed;
    const auto parsed = json::parse(Read(manifest));
    for (const auto& example : parsed.at("examples"))
        if (example.at("eligible").get<bool>())
            placed[example.at("id").get<std::string>()] = example.at("partition").get<std::string>();
    return placed;
}

struct Fixture
{
    revia::tests::ScopedTestDirectory directory;
    LiveCaptureStore store{directory.root / "Captures"};
    std::filesystem::path lineage = directory.root / "Datasets";
    std::filesystem::path parent = directory.root / "exports";
    Fixture() { std::filesystem::create_directories(parent); }
    bool Export(LiveDatasetExport& exported, std::string& error, const std::filesystem::path& folder = {}) const
    {
        return ExportLiveDataset(store, lineage, "revia-live", folder.empty() ? parent : folder, exported, error);
    }
};

void TestCandidateFileMatchesTheCaptureTool()
{
    revia::tests::ScopedTestDirectory directory;
    const LiveCaptureStore store(directory.root / "Captures");
    const auto capture = Read("Tests/Fixture/liveCapture.json");
    const auto id = json::parse(capture).at("captureId").get<std::string>();
    Write(store.LiveDirectory() / (id + ".json"), capture);
    std::string file, error;
    std::size_t candidates = 0, waiting = 0;
    Check(store.CandidateFile(file, candidates, waiting, error) && candidates == 0 && waiting == 1,
        "A capture without a saved correction was not counted as waiting: " + error);
    Write(store.CorrectionsDirectory() / (id + ".json"), Read("Tests/Fixture/liveCorrection.json"));
    Check(store.CandidateFile(file, candidates, waiting, error) && candidates == 1 && waiting == 0, error);
    // revia_dataset_capture.py candidates --prefix live wrote this from the same two fixtures.
    Check(json::parse(file) == json::parse(Read("Tests/Fixture/liveCandidates.json")),
        "The in-app candidates differ from what revia_dataset_capture.py writes for the same capture and correction.");

    auto forged = json::parse(Read("Tests/Fixture/liveCorrection.json"));
    forged["failedOutputSha256"] = revia::audit::ContentDigest("Another reply.");
    Write(store.CorrectionsDirectory() / (id + ".json"), forged.dump());
    Check(!store.CandidateFile(file, candidates, waiting, error) && error.find("another reply") != std::string::npos,
        "A correction bound to another reply became a candidate.");
}

void TestNumberedExportsKeepEveryEarlierPartition()
{
    Fixture fixture;
    for (int number = 1; number <= 30; ++number)
        Keep(fixture.store, number);
    Keep(fixture.store, 31, LearningDecision::Pending);
    Keep(fixture.store, 32, LearningDecision::Reject);
    LiveDatasetExport first;
    std::string error;
    Check(fixture.Export(first, error), "A first export of accepted corrections failed: " + error);
    Check(first.name == "revia-live-1" && first.number == 1 && first.directory == fixture.parent / "revia-live-1" &&
              first.train + first.validation + first.test == 30 && first.waiting == 1 && first.excluded.size() == 1 &&
              first.excluded[0].reason == "its review has not accepted it",
        "The first export did not hold the 30 accepted corrections, or misreported the pending and rejected ones.");
    Check(first.train > 0 && first.validation + first.test > 0, "Thirty independent examples were not split into partitions.");
    const auto firstPlaced = Partitions(first.directory / "manifest.json");
    Check(firstPlaced.size() == 30 && revia::audit::ContentDigest(Read(first.directory / "manifest.json")) == first.manifestSha256,
        "The first manifest does not record what was exported.");
    Check(std::filesystem::exists(first.withdrawnList) && json::parse(Read(first.withdrawnList)) == json::array(),
        "The export did not leave a withdrawal list for a training job to name.");

    const auto listedLater = Keep(fixture.store, 33);
    for (int number = 34; number <= 45; ++number)
        Keep(fixture.store, number);
    // Listed as withdrawn although its file is still kept, as when deleting it failed.
    Write(fixture.store.WithdrawnList(), json::array({"conversation:conversation-33"}).dump());
    LiveDatasetExport second;
    Check(fixture.Export(second, error), "A second export failed: " + error);
    Check(second.name == "revia-live-2" && second.train + second.validation + second.test == 42 &&
              std::any_of(second.excluded.begin(), second.excluded.end(),
                  [&](const DatasetDecision& decision)
                  { return decision.exampleId == "live:" + listedLater && decision.reason == "a source it used was withdrawn"; }),
        "The second export is not the next numbered one, or exported a capture listed as withdrawn.");
    const auto secondPlaced = Partitions(second.directory / "manifest.json");
    for (const auto& [id, partition] : firstPlaced)
        Check(secondPlaced.at(id) == partition, id + " moved from " + partition + " to " + secondPlaced.at(id) + " between exports.");
    const auto firstManifest = json::parse(Read(first.directory / "manifest.json"));
    const auto secondManifest = json::parse(Read(second.directory / "manifest.json"));
    Check(firstManifest["salt"] == secondManifest["salt"] && firstManifest["name"] == "revia-live-1" &&
              secondManifest["name"] == "revia-live-2",
        "The lineage changed its salt, or the exports do not name themselves.");
    const auto line = Read(second.directory / "train.jsonl");
    const auto record = json::parse(line.substr(0, line.find('\n')));
    Check(record["messages"].back()["role"] == "assistant" && record["messages"].back()["content"].get<std::string>().starts_with("A helpful") &&
              record.contains("rejected"),
        "An exported example is not the correction with its judged reply beside it.");
    Check(DescribeLiveDatasetExport(second).find("revia-live-2") != std::string::npos, "The summary does not name the export.");
}

void TestReturningExampleKeepsItsPartitionFromAnyEarlierExport()
{
    Fixture fixture;
    std::vector<std::string> kept;
    for (int number = 1; number <= 30; ++number)
        kept.push_back(Keep(fixture.store, number));
    LiveDatasetExport first;
    std::string error;
    Check(fixture.Export(first, error), error);
    const auto firstPlaced = Partitions(first.directory / "manifest.json");
    std::string heldOut;
    int heldOutNumber = 0;
    for (int number = 1; number <= 30 && heldOut.empty(); ++number)
        if (firstPlaced.at("live:" + kept[number - 1]) != "train")
        {
            heldOut = kept[number - 1];
            heldOutNumber = number;
        }
    Check(!heldOut.empty(), "No example was held out to test with.");

    // Deleted before the second export, which therefore no longer lists it.
    Check(fixture.store.Discard(heldOut, error), error);
    LiveDatasetExport second;
    Check(fixture.Export(second, error) && !Partitions(second.directory / "manifest.json").contains("live:" + heldOut),
        "A deleted capture was exported again: " + error);
    std::vector<std::string> stale;
    Check(StaleDatasetExamples(Read(first.directory / "manifest.json"), {"displayed-reply:" + std::to_string(heldOutNumber)}, stale, error) &&
              stale == std::vector<std::string>{"live:" + heldOut},
        "The earlier export was not flagged once its example was withdrawn.");

    // It comes back with a new turn of the same chat that sorts first, so the group's root, and
    // with it the hashed partition, change. Only the first export remembers where it was.
    Check(Keep(fixture.store, heldOutNumber, LearningDecision::Accept, "-again") == heldOut,
        "Judging the same reply again did not make the same capture.");
    int sibling = 1000;
    while (Judged(sibling, "", heldOutNumber).captureId > heldOut)
        ++sibling;
    const auto newTurn = Keep(fixture.store, sibling, LearningDecision::Accept, "", heldOutNumber);
    LiveDatasetExport third;
    Check(fixture.Export(third, error), error);
    const auto thirdPlaced = Partitions(third.directory / "manifest.json");
    Check(thirdPlaced.at("live:" + heldOut) == firstPlaced.at("live:" + heldOut) &&
              thirdPlaced.at("live:" + newTurn) == firstPlaced.at("live:" + heldOut),
        "A returning example, or the chat turn that joined it, left the held-out partition an earlier export gave it.");
}

void TestRefusalsWriteNothing()
{
    Fixture fixture;
    LiveDatasetExport exported;
    std::string error;
    Check(!fixture.Export(exported, error) && error.find("No exchange is kept") != std::string::npos, "An empty store exported.");
    Keep(fixture.store, 1, LearningDecision::Pending);
    Check(!fixture.Export(exported, error) && error.find("No kept capture has a decision yet") != std::string::npos,
        "Undecided captures exported.");
    Keep(fixture.store, 2, LearningDecision::Reject);
    Check(!fixture.Export(exported, error) && error.find("No accepted correction") != std::string::npos,
        "Only rejected captures exported.");
    Keep(fixture.store, 3);

    Check(!fixture.Export(exported, error, "relative/folder") && error.find("full path") != std::string::npos, "A relative folder was used.");
    Check(!fixture.Export(exported, error, fixture.parent / "missing") && error.find("does not exist") != std::string::npos,
        "A missing folder was used.");
    Check(!fixture.Export(exported, error, "//server/share") && error.find("network") != std::string::npos, "A network folder was used.");
    std::error_code linkError;
    std::filesystem::create_directory_symlink(fixture.parent, fixture.directory.root / "linked", linkError);
    if (!linkError)
        Check(!fixture.Export(exported, error, fixture.directory.root / "linked") && error.find("link") != std::string::npos,
            "A linked folder was used.");
    SetOneDrive(fixture.directory.root);
    const bool refused = !fixture.Export(exported, error) && error.find("OneDrive") != std::string::npos;
    SetOneDrive({});
    Check(refused, "A folder inside OneDrive was used for private training data.");
    Check(!std::filesystem::exists(fixture.lineage) && std::filesystem::is_empty(fixture.parent), "A refused export wrote something.");

    std::filesystem::create_directories(fixture.parent / "revia-live-1");
    Check(!fixture.Export(exported, error) && error.find("already exists") != std::string::npos, "An existing folder was replaced.");
    std::filesystem::remove(fixture.parent / "revia-live-1");
    Check(fixture.Export(exported, error) && exported.name == "revia-live-1", "A refused export used up a number: " + error);
    Write(fixture.lineage / "manifests" / "revia-live-1.json", "{}");
    Check(!fixture.Export(exported, error) && error.find("missing or changed") != std::string::npos &&
              !std::filesystem::exists(fixture.parent / "revia-live-2"),
        "An export ran without the earlier manifest its partitions come from.");
    Write(fixture.lineage / "lineage.json", "{broken");
    Check(!fixture.Export(exported, error) && error.find("lineage could not be read") != std::string::npos,
        "An unreadable lineage was reset instead of refusing.");
}
} // namespace

void RunLiveDatasetExportTests()
{
    std::string errors;
    for (const auto test : {TestCandidateFileMatchesTheCaptureTool, TestNumberedExportsKeepEveryEarlierPartition,
             TestReturningExampleKeepsItsPartitionFromAnyEarlierExport, TestRefusalsWriteNothing})
        try
        {
            test();
        }
        catch (const std::exception& error)
        {
            errors += std::string(error.what()) + '\n';
        }
    Check(errors.empty(), errors);
    std::cout << "Live dataset export parity, numbered lineage, kept partitions and refusals passed.\n";
}
