#include "testSupport.h"

#include "Library/structLibrary.h"
#include "Memory/longTermMemory.h"
#include "Memory/memoryReconciliation.h"
#include "promptLayoutTestSupport.h"
#include "reviaSessionTestAccess.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

// A correction supersedes what it corrects instead of sitting beside it.
//
// ISSUE-REVIA-0080: "The user likes coffee" and "The user no longer likes coffee" were
// both kept and both offered, in an order decided by ranking. Now the later one, when it
// states a change and contradicts the earlier, marks it as past. Nothing is deleted by
// that; the owner's forget is the only delete, and it is tested here too, with the
// incognito switch that keeps a conversation out of every store.
namespace
{
using namespace std::chrono_literals;
using revia::memory::StatesAChange;
using revia::runtime::ReviaSession;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
using Access = revia::runtime::ReviaSessionTestAccess;
using json = nlohmann::json;

memoryDecision Decision(const std::string& summary, std::vector<float> embedding = {})
{
    memoryDecision decision;
    decision.bSuccess = decision.bShouldRemember = true;
    decision.category = "preference";
    decision.summary = summary;
    if (!embedding.empty())
    {
        decision.embedding = std::move(embedding);
        decision.embeddingModel = "fixture";
    }
    return decision;
}

// A vector at a chosen angle from (1,0), so a case can state the similarity it means to
// test instead of depending on a real embedding model's geometry.
std::vector<float> AtSimilarity(const double cosine)
{
    return {static_cast<float>(cosine),
        static_cast<float>(std::sqrt(std::max(0.0, 1.0 - cosine * cosine)))};
}

std::optional<memoryEntry> Stored(const longTermMemory& store, const std::string& id)
{
    return store.Find(id);
}

void TestStatingAChangeIsNarrowerThanContradicting()
{
    for (const char* change : {
             "The user no longer likes coffee.", "The user switched from Vim to Emacs.",
             "The user used to live in Austin.", "The user now prefers tea.",
             "The user stopped playing chess.", "The user doesn't drink coffee anymore."})
    {
        Check(StatesAChange(change), std::string("Not read as a change: ") + change);
    }
    for (const char* steady : {
             "The user prefers dark themes.", "The user dislikes cilantro.",
             "The user would rather use MinGW than MSVC.", "The user never eats fish.",
             "The user is not a morning person.", "The user is now 34 years old."})
    {
        Check(!StatesAChange(steady), std::string("Read as a change: ") + steady);
    }
}

void TestACorrectionSupersedesWhatItContradicts()
{
    ScopedTestDirectory directory;
    longTermMemory store((directory.root / "memory.db").string());
    bool added = false;
    std::string older, newer, superseded;
    Check(store.Save(Decision("The user likes coffee.", AtSimilarity(1.0)), added, &older) &&
              added,
        "The first memory was not saved.");
    Check(store.Save(Decision("The user no longer likes coffee.", AtSimilarity(0.97)),
              added, &newer, &superseded) && added,
        "The correction was not saved.");
    Check(superseded == older, "The correction did not name what it corrected.");

    const auto corrected = Stored(store, older);
    Check(corrected && !corrected->Current() && corrected->supersededBy == newer &&
              !corrected->validTo.empty(),
        "The corrected memory was not marked as past.");
    Check(store.Load().size() == 2, "A correction deleted what it corrected.");

    // The correction is what recall offers, and it carries what it replaced.
    const std::string block = store.BuildPromptBlock("coffee", 6, AtSimilarity(0.98), "fixture");
    Check(block.find("no longer likes coffee") != std::string::npos &&
              block.find("corrected an earlier note: \"The user likes coffee.\"") !=
                  std::string::npos,
        "Recall did not say what changed: " + block);
    Check(block.find("- [preference] (a few minutes ago) The user likes coffee.") ==
              std::string::npos,
        "The corrected memory was still offered as current.");

    // A second correction supersedes the first correction, not the original again.
    std::string third, supersededAgain;
    Check(store.Save(Decision("The user now likes coffee again.", AtSimilarity(0.96)),
              added, &third, &supersededAgain) && added,
        "A second correction was not saved.");
    Check(supersededAgain == newer, "The second correction did not supersede the first.");
    Check(Stored(store, newer)->supersededBy == third && Stored(store, older)->supersededBy == newer,
        "The chain of corrections was not kept.");

    // A restart sees the same.
    longTermMemory reopened((directory.root / "memory.db").string());
    const auto entries = reopened.Load();
    Check(entries.size() == 3 &&
              std::count_if(entries.begin(), entries.end(),
                  [](const memoryEntry& entry) { return entry.Current(); }) == 1,
        "The corrections did not survive a reopen.");
}

void TestNothingSupersedesWithoutAStatedChange()
{
    ScopedTestDirectory directory;
    longTermMemory store((directory.root / "memory.db").string());
    bool added = false;
    std::string first, second, superseded;
    Check(store.Save(Decision("The user prefers dark themes.", AtSimilarity(1.0)), added, &first),
        "The first memory was not saved.");

    // An opposite with no change stated: either could be the stale one.
    Check(store.Save(Decision("The user prefers light themes.", AtSimilarity(0.998)),
              added, &second, &superseded) && added && superseded.empty(),
        "An opposite preference superseded the earlier one without saying anything changed.");
    // A contradiction by the classifier's reading that is really a preference
    // (ISSUE-REVIA-0088).
    Check(store.Save(Decision("The user would rather use light themes than dark ones.",
              AtSimilarity(0.99)), added, &second, &superseded) && superseded.empty(),
        "\"would rather\" retired an earlier memory.");
    // A change about something else entirely: the vectors are far apart.
    Check(store.Save(Decision("The user no longer plays chess.", AtSimilarity(0.3)),
              added, &second, &superseded) && superseded.empty(),
        "A change about another topic retired an unrelated memory.");
    // A change with no vector to place it by.
    Check(store.Save(Decision("The user no longer likes dark themes.", {}),
              added, &second, &superseded) && superseded.empty(),
        "A memory with no vector superseded on a similarity nobody measured.");
    const std::vector<memoryEntry> entries = store.Load();
    Check(entries.size() == 5 && std::all_of(entries.begin(), entries.end(),
              [](const memoryEntry& entry) { return entry.Current(); }),
        "Something was marked as past without grounds.");
}

void TestARecalledTimeStillReachesWhatWasTrueThen()
{
    ScopedTestDirectory directory;
    longTermMemory store((directory.root / "memory.db").string());
    bool added = false;
    std::string older, newer;
    Check(store.Save(Decision("The user drinks coffee every morning.", AtSimilarity(1.0)),
              added, &older),
        "The first memory was not saved.");
    Check(store.Save(Decision("The user switched to tea in the mornings.", AtSimilarity(0.95)),
              added, &newer) && Stored(store, older)->supersededBy == newer,
        "The switch did not supersede the coffee memory.");
    // Recall by time reaches the past, and says it is past.
    const std::string today = store.BuildPromptBlock("what did I tell you today", 6);
    Check(today.find("since corrected) The user drinks coffee every morning.") != std::string::npos,
        "A question naming today did not reach the corrected memory, or did not mark it: " + today);
    // Recall by topic does not offer it as current.
    const std::string topic = store.BuildPromptBlock("coffee", 6, AtSimilarity(0.99), "fixture");
    Check(topic.find("switched to tea") != std::string::npos &&
              topic.find("(a few minutes ago) The user drinks coffee every morning.") ==
                  std::string::npos,
        "Recall by topic offered a corrected memory as current: " + topic);
}

void TestForgettingRemovesForGoodAndRestoresWhatItCorrected()
{
    ScopedTestDirectory directory;
    longTermMemory store((directory.root / "memory.db").string());
    bool added = false;
    std::string older, newer;
    Check(store.Save(Decision("The user likes jazz.", AtSimilarity(1.0)), added, &older),
        "The first memory was not saved.");
    Check(store.Save(Decision("The user no longer likes jazz.", AtSimilarity(0.97)),
              added, &newer) && Stored(store, older)->supersededBy == newer,
        "The correction did not supersede.");
    Check(!store.Forget("memory-that-does-not-exist"), "Forgetting nothing reported success.");
    Check(store.Forget(newer), "The correction could not be forgotten.");
    Check(!Stored(store, newer), "A forgotten memory was still there.");
    Check(Stored(store, older) && Stored(store, older)->Current(),
        "Forgetting a correction left what it corrected marked as past by nothing.");
    Check(store.Search("jazz", 6).size() == 1 && store.Search("jazz", 6).front().id == older,
        "The search index still held the forgotten memory, or lost the restored one.");
    Check(store.Forget(older) && store.Load().empty(), "The last memory could not be forgotten.");
    Check(store.Search("jazz", 6, AtSimilarity(1.0), "fixture").empty(),
        "A vector survived the memory it belonged to.");
}

// Incognito through a real session: turns are not archived and the record is not
// persisted, and she is told so.
class QuietBackend
{
public:
    QuietBackend()
    {
        server.Get("/health", [](const auto&, auto& response)
        { response.set_content(R"({"status":"ok","slots_idle":1})", "application/json"); });
        server.Get("/v1/models", [](const auto&, auto& response)
        { response.set_content(R"({"data":[{"id":"fixture-main"}]})", "application/json"); });
        server.Get("/props", [](const auto&, auto& response)
        {
            response.set_content(
                R"({"total_slots":1,"default_generation_settings":{"n_ctx":8192}})",
                "application/json");
        });
        server.Post("/v1/chat/completions", [this](const auto& request, auto& response)
        {
            const json body = json::parse(request.body);
            {
                std::lock_guard lock(mutex);
                lastRequest = body;
            }
            const std::string answer = "Understood.";
            if (body.value("stream", false))
            {
                const json chunk = {{"choices", json::array({{
                    {"delta", {{"content", answer}}}, {"finish_reason", "stop"}}})}};
                response.set_content(
                    "data: " + chunk.dump() + "\n\ndata: [DONE]\n\n", "text/event-stream");
            }
            else
            {
                response.set_content(json{{"choices", json::array({{
                    {"message", {{"role", "assistant"}, {"content", answer}}},
                    {"finish_reason", "stop"}}})}}.dump(), "application/json");
            }
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind the incognito fixture backend.");
        thread = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        Check(server.is_running(), "The incognito fixture backend did not start.");
    }
    ~QuietBackend() { server.stop(); thread.join(); }
    json LastRequest() { std::lock_guard lock(mutex); return lastRequest; }
    int port = 0;
private:
    httplib::Server server;
    std::mutex mutex;
    json lastRequest;
    std::jthread thread;
};

class WorkingDirectory
{
public:
    explicit WorkingDirectory(const std::filesystem::path& root)
        : previous(std::filesystem::current_path()) { std::filesystem::current_path(root); }
    ~WorkingDirectory() { std::filesystem::current_path(previous); }
private:
    std::filesystem::path previous;
};

void Write(const std::filesystem::path& path, const json& value)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << value.dump(2);
    output.close();
    Check(!output.fail(), "Could not write the incognito fixture.");
}

void TestIncognitoKeepsAConversationOutOfEveryStore()
{
    ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    QuietBackend backend;
    Write(directory.root / "Config/settings.json", {
        {"activeProfile", "fixture"},
        {"llm", {{"backend", "LLamaCpp"}, {"host", "127.0.0.1"}, {"port", backend.port},
            {"modelName", "fixture-main"}, {"autoStartServer", false}, {"visionEnabled", false},
            {"maxTokens", 256}, {"autoMaxTokens", false},
            {"modelPath", (directory.root / "absent.gguf").string()},
            {"mediaPath", (directory.root / "RuntimeData/Vision").string()}}},
        {"intelligence", {{"enabled", false}}},
        {"embedding", {{"enabled", false}, {"autoStartServer", false}}},
        {"speech", {{"enabled", false}, {"backend", "WindowsSapi"}, {"speakGreeting", false},
            {"voiceDataPath", (directory.root / "RuntimeData/Voices").string()}}},
        {"speechRecognition", {{"enabled", false}}},
        {"presence", {{"enabled", false}, {"avatarBridgeEnabled", false},
            {"externalAdaptersEnabled", false},
            {"statePath", (directory.root / "Presence/state.json").string()},
            {"eventPath", (directory.root / "Presence/events.jsonl").string()},
            {"inboxPath", (directory.root / "Presence/Inbox").string()},
            {"outboxPath", (directory.root / "Presence/Outbox").string()}}},
        {"vision", {{"enabled", false}}},
        {"perception", {{"enabled", false}}}, {"initiative", {{"enabled", false}}},
        {"bargeIn", {{"enabled", false}}},
        {"conversation", {{"archiveEnabled", true}, {"restoreTurns", 6},
            {"historyCompactionEnabled", false}, {"selfInquiryEnabled", false}}},
        {"image", {{"enabled", false}}}, {"resources", {{"startupSampleSeconds", 0}}},
        {"responseFilter", {{"aiReviewEnabled", false}}}
    });
    Write(directory.root / "Config/Profiles/fixture.json", {
        {"id", "fixture"}, {"displayName", "Fixture"},
        {"systemPrompt", "You are Revia. Incognito fixture."},
        {"shouldSpeak", false}, {"memoryEnabled", false}
    });

    ReviaSession session;
    Check(session.Start(), "The incognito session did not start.");
    Check(!session.IsIncognito(), "A session started in incognito.");
    Check(session.Submit("ARCHIVED_MARK: this one is kept.").succeeded, "A turn failed.");
    Check(session.SearchConversations("ARCHIVED_MARK").size() == 1,
        "An ordinary turn was not archived.");

    const auto switchedOn = session.Submit("/incognito on");
    Check(switchedOn.succeeded && session.IsIncognito() &&
              switchedOn.text.find("Incognito is on") != std::string::npos,
        "/incognito on did not switch it on.");
    Check(session.Submit("QUIET_MARK: this one must not be kept.").succeeded,
        "An incognito turn failed.");
    Check(session.SearchConversations("QUIET_MARK").empty(),
        "An incognito turn was archived.");
    Check(revia::tests::RuntimeAuthoredText(backend.LastRequest()).find("Incognito is on") !=
              std::string::npos,
        "She was not told that incognito is on.");

    Check(session.Submit("/incognito off").succeeded && !session.IsIncognito(),
        "/incognito off did not switch it off.");
    Check(session.Submit("BACK_MARK: kept again.").succeeded &&
              session.SearchConversations("BACK_MARK").size() == 1,
        "Archiving did not resume after incognito.");
    Check(revia::tests::RuntimeAuthoredText(backend.LastRequest()).find("Incognito is on") ==
              std::string::npos,
        "She was still told incognito was on after it was switched off.");
    session.Stop();
}
} // namespace

void RunMemorySupersessionTests()
{
    TestStatingAChangeIsNarrowerThanContradicting();
    TestACorrectionSupersedesWhatItContradicts();
    TestNothingSupersedesWithoutAStatedChange();
    TestARecalledTimeStillReachesWhatWasTrueThen();
    TestForgettingRemovesForGoodAndRestoresWhatItCorrected();
    TestIncognitoKeepsAConversationOutOfEveryStore();
    std::cout << "A stated change supersedes the memory it contradicts and is rendered as "
                 "used-to/now; nothing else marks a memory as past; forgetting is the only "
                 "delete; and incognito keeps a conversation out of every store.\n";
}
