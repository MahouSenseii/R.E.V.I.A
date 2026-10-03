#include "Runtime/companion.h"
#include "Agents/memoryAgent.h"
#include "Agents/turnCoordinator.h"
#include "Core/configManager.h"
#include "Core/logger.h"
#include "Core/profile.h"
#include "Identity/identityStore.h"
#include "Goals/goalStore.h"
#include "LLM/promptBuilder.h"
#include "Memory/conversationArchive.h"
#include "Memory/longTermMemory.h"
#include "memoryAgentTestAccess.h"
#include "testSupport.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <atomic>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <system_error>
#include <thread>

namespace
{
using namespace revia::runtime;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
namespace fs = std::filesystem;
using json = nlohmann::json;
const CompanionDescriptor Legacy{"legacy", "Revia", "assistant", true};
void Write(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << text;
    file.close();
    Check(!file.fail(), "Synthetic fixture write failed.");
}
std::string Read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
memoryDecision Finding(const std::string& summary)
{
    memoryDecision decision;
    decision.bSuccess = true;
    decision.bShouldRemember = true;
    decision.summary = summary;
    decision.category = "fact";
    return decision;
}
void Save(longTermMemory& memory, const std::string& summary)
{
    bool added = false;
    Check(memory.Save(Finding(summary), added) && added, "Synthetic memory owner save failed.");
}
bool Rejected(const std::function<void()>& operation)
{
    try
    {
        operation();
        return false;
    }
    catch (const std::exception&)
    {
        return true;
    }
}

void TestCapturedPathsAndRegistry()
{
    ScopedTestDirectory directory;
    const auto root = fs::absolute(directory.root);
    CompanionPaths legacy(root, Legacy), a(root, {"A", "Alice", "assistant", false}), b(root, {"B", "Bob", "assistant", false});
    Check(legacy.Root() == root && a.Root() != b.Root() && a.Resolve("Memory/revia_memory.db").is_absolute(),
        "Companion roots were not absolute and disjoint.");
    for (const auto& id : {"../escape", "CON", "A:B", "", "legacy"})
        Check(Rejected([&] { CompanionPaths invalid(root, {id, "Bad", "assistant", false}); }), "Unsafe companion ID accepted.");
    Check(Rejected([&] { CompanionPaths invalid("relative", Legacy); }), "Relative install root accepted.");
    Check(Rejected([&] { (void)a.Resolve("../B/secret"); }) && Rejected([&] { (void)a.Resolve("Logs/stream:payload"); }),
        "Path traversal or alternate stream accepted.");
    Check(Rejected([&] { (void)a.Resolve("Logs/NUL.txt"); }) && Rejected([&] { (void)a.Resolve("Logs/.. /outside"); }),
        "Windows device/normalized traversal accepted.");
    CompanionRegistry registry(root);
    std::string error;
    Check(
        registry.Load(error) && registry.List().size() == 1 && registry.SelectedId() == "legacy", "Explicit legacy registration missing.");
    CompanionDescriptor created;
    Check(registry.Create("Empty mind", "assistant.v2", created, error), error);
    Check(created.id.starts_with("c-") && !created.legacy && !fs::exists(CompanionPaths(root, created).Root()),
        "New companion cloned or initialized private data.");
    Check(registry.Select(created.id, error), error);
    CompanionRegistry reopened(root);
    Check(reopened.Load(error) && reopened.SelectedId() == created.id && reopened.Find(created.id)->profileId == "assistant.v2",
        "Registry selection did not survive restart.");
    Check(!reopened.Select("missing", error), "Unknown companion selection persisted.");
    Write(root / "RuntimeData/Companions/registry.json", R"({"schemaVersion":99,"selectedId":"legacy","companions":[]})");
    Check(!reopened.Load(error) && !reopened.Create("Unsafe overwrite", "assistant", created, error), "Newer registry was overwritten.");
}

void TestNeutralProfilesAndPrivateSaves()
{
    ScopedTestDirectory directory;
    Write(directory.root / "neutral/assistant.json", R"({"id":"assistant","displayName":"Neutral","systemPrompt":"Neutral seed"})");
    configManager a(
        (directory.root / "settings.json").string(), (directory.root / "A/Profiles").string(), (directory.root / "neutral").string());
    configManager b(
        (directory.root / "settings.json").string(), (directory.root / "B/Profiles").string(), (directory.root / "neutral").string());
    aiProfile profile;
    std::string error;
    Check(a.LoadProfile("assistant", profile) && profile.displayName == "Neutral", "Neutral seed fallback failed.");
    profile.displayName = "A authored";
    Check(a.SaveProfile(profile, error), error);
    aiProfile other;
    Check(b.LoadProfile("assistant", other) && other.displayName == "Neutral", "Private profile save contaminated another companion.");
    Check(a.LoadProfile("assistant", other) && other.displayName == "A authored" && a.ListProfiles().size() == 1,
        "Private override or deduplicated profile list failed.");
    Check(Read(directory.root / "neutral/assistant.json").find("A authored") == std::string::npos, "Neutral profile was modified.");
    Write(directory.root / "A/Profiles/assistant.json", "{broken");
    Check(!a.LoadProfile("assistant", other), "Corrupt private profile silently fell back to seed.");
}

void TestConcurrentActualOwners()
{
    ScopedTestDirectory directory;
    CompanionPaths a(directory.root, {"A", "A", "assistant", false}), b(directory.root, {"B", "B", "assistant", false});
    const auto cwd = fs::current_path();
    longTermMemory memoryA(a.Resolve("Memory/revia_memory.db").string()), memoryB(b.Resolve("Memory/revia_memory.db").string());
    auto first = std::async(std::launch::async, [&] { Save(memoryA, "A remembers the sapphire teapot."); });
    auto second = std::async(std::launch::async, [&] { Save(memoryB, "B remembers the amber lantern."); });
    first.get();
    second.get();
    promptBuilder promptA(a.Resolve("Memory/revia_memory.db").string()), promptB(b.Resolve("Memory/revia_memory.db").string());
    Check(promptA.BuildMemoryBlock().find("sapphire teapot") != std::string::npos &&
              promptA.BuildMemoryBlock().find("amber lantern") == std::string::npos,
        "A prompt crossed memory ownership.");
    Check(promptB.BuildMemoryBlock().find("amber lantern") != std::string::npos &&
              promptB.BuildMemoryBlock().find("sapphire teapot") == std::string::npos,
        "B prompt crossed memory ownership.");
    logger logA(a.Resolve("Logs")), logB(b.Resolve("Logs"));
    auto la = std::async(std::launch::async,
        [&]
        {
            for (int i = 0; i < 4; ++i)
                logA.Log("A-private-log");
        });
    auto lb = std::async(std::launch::async,
        [&]
        {
            for (int i = 0; i < 4; ++i)
                logB.Log("B-private-log");
        });
    la.get();
    lb.get();
    Check(Read(a.Resolve("Logs/revia.log")).find("B-private-log") == std::string::npos &&
              Read(b.Resolve("Logs/revia.log")).find("A-private-log") == std::string::npos,
        "Captured log streams crossed companion roots.");
    Check(fs::current_path() == cwd, "Concurrent owner fixture switched process cwd.");
}

void TestAllRouterTiersUseCapturedMemory()
{
    ScopedTestDirectory directory;
    const auto db = directory.root / "A/Memory/revia_memory.db";
    longTermMemory memory(db.string());
    Save(memory, "Only A knows the cobalt compass.");
    httplib::Server server;
    std::mutex mutex;
    std::vector<std::string> requests;
    server.Get("/health",
        [](const httplib::Request&, httplib::Response& response) { response.set_content(R"({"status":"ok"})", "application/json"); });
    server.Get("/v1/models", [](const httplib::Request&, httplib::Response& response)
        { response.set_content(R"({"data":[{"id":"fixture-main"},{"id":"fixture-fast"},{"id":"fixture-expert"}]})", "application/json"); });
    server.Get("/props", [](const httplib::Request&, httplib::Response& response)
        { response.set_content(R"({"total_slots":2,"default_generation_settings":{"n_ctx":8192}})", "application/json"); });
    server.Post("/v1/chat/completions",
        [&](const httplib::Request& request, httplib::Response& response)
        {
            {
                std::lock_guard lock(mutex);
                requests.push_back(request.body);
            }
            response.set_content("data: {\"choices\":[{\"delta\":{\"content\":\"Fixture reply.\"},\"finish_reason\":null}]}\n\ndata: "
                                 "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n",
                "text/event-stream");
        });
    const int port = server.bind_to_any_port("127.0.0.1");
    Check(port > 0, "Loopback port unavailable.");
    std::jthread listening([&] { server.listen_after_bind(); });
    struct Stop
    {
        httplib::Server& server;
        ~Stop()
        {
            server.stop();
        }
    } stop{server};
    messageRouter router(db.string());
    llmSettings main, fast, expert;
    main.port = port;
    main.modelName = "fixture-main";
    main.device = "none";
    main.bVisionEnabled = false;
    fast = main;
    expert = main;
    fast.modelName = "fixture-fast";
    expert.modelName = "fixture-expert";
    embeddingSettings embeddings;
    embeddings.bEnabled = false;
    aiProfile profile;
    router.ApplyLLMSettings(main, fast, expert, embeddings, profile, true, true);
    for (const auto tier : {revia::intelligence::IntelligenceTier::Main, revia::intelligence::IntelligenceTier::Fast,
             revia::intelligence::IntelligenceTier::Expert})
    {
        revia::intelligence::IntelligenceDecision decision;
        decision.selectedTier = tier;
        const auto result =
            router.RouteMessage("Tell me about my cobalt compass.", {{"user", "Tell me about my cobalt compass."}}, {}, {}, decision);
        Check(result.bSuccess, "Actual tier request failed on controlled loopback: " + result.reason);
    }
    Check(requests.size() == 3, "Actual three-tier request count differed.");
    for (std::size_t i = 0; i < requests.size(); ++i)
    {
        Check(requests[i].find("cobalt compass") != std::string::npos && requests[i].find("Only A knows") != std::string::npos,
            "A router tier bypassed the captured private memory path.");
        const auto model = json::parse(requests[i]).at("model").get<std::string>();
        Check(model == std::vector<std::string>{"fixture-main", "fixture-fast", "fixture-expert"}[i], "Requested tier was not exercised.");
    }
}

void TestStaleQueuedEvaluationAndLearnedFinding()
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "memory.db").string();
    messageRouter router(path);
    std::atomic<bool> current = true;
    std::mutex mutex;
    std::condition_variable entered;
    bool evaluating = false, release = false, sentinel = false;
    revia::agents::MemoryAgent agent(path);
    struct Release
    {
        std::mutex& mutex;
        std::condition_variable& entered;
        bool& release;
        ~Release()
        {
            std::lock_guard lock(mutex);
            release = true;
            entered.notify_all();
        }
    } releaseOnFailure{mutex, entered, release};
    agent.SetAdmissionGuard([&] { return current.load(); });
    revia::agents::MemoryAgentTestAccess::SetEvaluator(agent,
        [&](const std::string& input, const std::string&)
        {
            if (input == "sentinel")
            {
                std::lock_guard lock(mutex);
                sentinel = true;
                entered.notify_all();
                return memoryDecision{};
            }
            std::unique_lock lock(mutex);
            evaluating = true;
            entered.notify_all();
            entered.wait(lock, [&] { return release; });
            return Finding("Stale evaluation must never persist.");
        });
    agent.Submit(router, "A old turn", "A old reply", revia::agents::ResponseProvenance::NormalGeneration, 42);
    {
        std::unique_lock lock(mutex);
        Check(entered.wait_for(lock, std::chrono::seconds(3), [&] { return evaluating; }), "Controlled evaluator did not begin.");
    }
    current = false;
    // Rebinding is intentionally allowed, but must not bless an already-started task.
    agent.SetAdmissionGuard([] { return true; });
    agent.Submit(router, "sentinel", "", revia::agents::ResponseProvenance::NormalGeneration, 43);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    entered.notify_all();
    {
        std::unique_lock lock(mutex);
        Check(entered.wait_for(lock, std::chrono::seconds(3), [&] { return sentinel; }), "Sentinel did not prove prior task completion.");
    }
    agent.Stop();
    Check(longTermMemory(path).Load().empty(), "Stale task persisted after its submission guard was revoked and owner rebound.");
    const auto events = agent.DrainEvents();
    Check(std::none_of(events.begin(), events.end(), [](const auto& event) { return event.turnId == 42; }),
        "Stale task published a callback after invalidation.");
    revia::agents::TurnCoordinator coordinator(path);
    coordinator.SetAdmissionGuard([] { return false; });
    Check(coordinator.SubmitLearnedFinding(router, Finding("Stale approved finding")) == revia::agents::LearnedFindingResult::Failed,
        "Coordinator admission did not reach approved memory commit.");
    coordinator.Stop();
    Check(longTermMemory(path).Load().empty(), "Revoked approved finding was saved.");
}

void SeedLegacy(const CompanionPaths& source)
{
    longTermMemory memory(source.Resolve("Memory/revia_memory.db").string());
    Save(memory, "Legacy synthetic keeps its stable sapphire memory.");
    const auto entry = memory.Load().front();
    Check(memory.SaveEmbedding(entry.id, "fixture-vector", {0.25F, 0.75F}), "Synthetic embedding write failed.");
    revia::memory::ConversationArchive archive(source.Resolve("Memory/revia_conversations.db").string());
    std::string error;
    Check(archive.BeginSession("stable-session", error) &&
              archive.Record("stable-session", "user", "Synthetic archived conversation.", error),
        error);
    revia::goals::Goal goal;
    goal.id = "stable-goal";
    goal.title = "Synthetic goal";
    Check(revia::goals::GoalStore(source.Resolve("Goals/revia_goals.db").string()).Save(goal), "Synthetic goal save failed.");
    revia::identity::IdentityStore identity(source.Resolve("RuntimeData/Identity/identity.json"));
    revia::identity::IdentitySnapshot state;
    Check(identity.Save(state, error), error);
    Write(source.Resolve("RuntimeData/Preferences/preferences.json"), R"({"language":"fixture-only"})");
    Write(source.Resolve("RuntimeData/Capabilities/machine.json"), R"({"mustNotMigrate":true})");
    Write(source.Resolve("Models/never-copy.gguf"), "not a model");
}

void TestStaleBackfillCannotCommitOrPublish()
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "memory.db").string();
    longTermMemory memory(path);
    Save(memory, "Synthetic row awaiting its vector.");
    const auto id = memory.Load().front().id;
    std::mutex mutex;
    std::condition_variable changed;
    bool arrived = false, release = false, sentinel = false;
    std::atomic<bool> current = true;
    httplib::Server server;
    server.Post("/v1/embeddings",
        [&](const httplib::Request&, httplib::Response& response)
        {
            std::unique_lock lock(mutex);
            arrived = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
            response.set_content(R"({"data":[{"embedding":[0.25,0.75]}]})", "application/json");
        });
    const int port = server.bind_to_any_port("127.0.0.1");
    Check(port > 0, "Backfill loopback unavailable.");
    std::jthread listening([&] { server.listen_after_bind(); });
    struct StopServer
    {
        httplib::Server& server;
        ~StopServer()
        {
            server.stop();
        }
    } stopServer{server};
    messageRouter router(path);
    llmSettings settings;
    embeddingSettings embeddings;
    embeddings.port = port;
    embeddings.modelName = "fixture-vector";
    aiProfile profile;
    router.ApplyLLMSettings(settings, embeddings, profile);
    revia::agents::MemoryAgent agent(path);
    agent.SetAdmissionGuard([&] { return current.load(); });
    struct Release
    {
        std::mutex& mutex;
        std::condition_variable& changed;
        bool& release;
        ~Release()
        {
            std::lock_guard lock(mutex);
            release = true;
            changed.notify_all();
        }
    } releaseOnFailure{mutex, changed, release};
    revia::agents::MemoryAgentTestAccess::SetEvaluator(agent,
        [&](const std::string&, const std::string&)
        {
            std::lock_guard lock(mutex);
            sentinel = true;
            changed.notify_all();
            return memoryDecision{};
        });
    agent.SubmitEmbeddingBackfill(router, "fixture-vector");
    {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock, std::chrono::seconds(3), [&] { return arrived; }), "Actual backfill HTTP request did not start.");
    }
    current = false;
    agent.SetAdmissionGuard([] { return true; });
    agent.Submit(router, "sentinel", "", revia::agents::ResponseProvenance::NormalGeneration, 99);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock, std::chrono::seconds(3), [&] { return sentinel; }), "Sentinel did not prove backfill completion.");
    }
    agent.Stop();
    Check(memory.NeedsEmbedding(id, "fixture-vector"), "Stale real backfill committed an embedding after admission changed.");
    const auto events = agent.DrainEvents();
    Check(std::none_of(events.begin(), events.end(), [](const auto& event) { return event.operation == "memory_backfill"; }),
        "Stale backfill published an event.");
}

void TestStorageAwareResumableMigration()
{
    ScopedTestDirectory directory;
    CompanionPaths source(directory.root, Legacy), target(directory.root, {"imported", "Imported", "assistant", false});
    SeedLegacy(source);
    // Keep a real WAL writer open and commit a record which main-file-only copying loses.
    sqlite3* database = nullptr;
    Check(sqlite3_open(source.Resolve("Memory/revia_memory.db").string().c_str(), &database) == SQLITE_OK, "WAL fixture open failed.");
    struct Close
    {
        sqlite3* value;
        ~Close()
        {
            sqlite3_close(value);
        }
    } close{database};
    Check(sqlite3_exec(database,
              "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0; INSERT INTO memories VALUES ('wal-stable-id','fact','Committed only "
              "in WAL.','committed only in wal.','fixture','123456',1);",
              nullptr, nullptr, nullptr) == SQLITE_OK,
        "WAL fixture write failed.");
    int copies = 0;
    auto first = MigrateLegacyCompanion(
        source, target, [&](auto phase, const auto&) { return phase != CompanionMigrationPhase::Copy || ++copies < 2; });
    Check(first.interrupted && !first.complete && first.copiedArtifacts == 1 && !fs::exists(target.Root()),
        "Interrupted copy exposed a partial companion.");
    auto resumed = MigrateLegacyCompanion(source, target);
    Check(resumed.complete, resumed.error);
    const auto original = longTermMemory(source.Resolve("Memory/revia_memory.db").string()).Load();
    const auto imported = longTermMemory(target.Resolve("Memory/revia_memory.db").string()).Load();
    Check(original.size() == 2 && imported.size() == 2, "SQLite backup lost committed WAL rows.");
    for (const auto& entry : original)
        Check(std::any_of(imported.begin(), imported.end(), [&](const auto& item)
                  { return item.id == entry.id && item.summary == entry.summary && item.createdAt == entry.createdAt; }),
            "Migration changed stable memory identity or content.");
    const auto embedded = std::find_if(original.begin(), original.end(), [](const auto& entry) { return entry.id != "wal-stable-id"; });
    Check(embedded != original.end() &&
              !longTermMemory(target.Resolve("Memory/revia_memory.db").string()).NeedsEmbedding(embedded->id, "fixture-vector"),
        "Migration lost embedding vectors.");
    Check(revia::goals::GoalStore(target.Resolve("Goals/revia_goals.db").string()).Load("stable-goal").has_value(),
        "Migration lost stable goals.");
    Check(
        revia::memory::ConversationArchive(target.Resolve("Memory/revia_conversations.db").string()).LoadSession("stable-session").size() ==
            1,
        "Migration lost conversation archive.");
    Check(!fs::exists(target.Resolve("RuntimeData/Capabilities/machine.json")) && !fs::exists(target.Resolve("Models/never-copy.gguf")),
        "Machine ceiling/model files entered private migration.");
    Check(fs::exists(source.Resolve("Memory/revia_memory.db")) && fs::exists(source.Resolve("RuntimeData/Identity/identity.json")),
        "Migration removed legacy data.");
    Check(!MigrateLegacyCompanion(source, target).complete, "Migration replaced an occupied private root.");
}

void TestMigrationRejectsCorruptNewerAndStorageFailures()
{
    ScopedTestDirectory directory;
    CompanionPaths source(directory.root, Legacy);
    SeedLegacy(source);
    const auto target = [&](const std::string& id) { return CompanionPaths(directory.root, {id, id, "assistant", false}); };
    const auto identity = source.Resolve("RuntimeData/Identity/identity.json");
    const auto saved = Read(identity);
    Write(identity, R"({"schemaVersion":99})");
    const auto newer = MigrateLegacyCompanion(source, target("newer"));
    Check(!newer.complete && !fs::exists(target("newer").Root()), "Newer identity was activated.");
    Write(identity, "{corrupt");
    Check(!MigrateLegacyCompanion(source, target("corrupt")).complete, "Corrupt identity migrated.");
    Write(identity, saved);
    auto interrupted = MigrateLegacyCompanion(
        source, target("changed"), [](auto phase, const auto&) { return phase != CompanionMigrationPhase::Publish; });
    Check(interrupted.interrupted, "Publish interruption fixture did not stop.");
    Write(source.Resolve("RuntimeData/Preferences/preferences.json"), R"({"language":"changed"})");
    Check(!MigrateLegacyCompanion(source, target("changed")).complete && !fs::exists(target("changed").Root()),
        "Changed source silently reused old staging.");
    auto storage = target("storage");
    Write(storage.Root().parent_path(), "blocking-file");
    Check(!MigrateLegacyCompanion(source, storage).complete && !fs::exists(storage.Root()),
        "Storage failure published an active partial root.");
    auto injected = target("injected");
    Check(MigrateLegacyCompanion(source, injected, [](auto phase, const auto&) { return phase != CompanionMigrationPhase::Publish; })
              .interrupted,
        "Staging injection setup failed.");
    auto stage = injected.Root();
    stage += ".migration";
    Write(stage / "RuntimeData/Capabilities/injected.json", "{}");
    Check(!MigrateLegacyCompanion(source, injected).complete, "Unexpected authority artifact in staging was published.");
}

void TestMigrationRevalidatesAtPublication()
{
    ScopedTestDirectory directory;
    CompanionPaths source(directory.root, Legacy), target(directory.root, {"publication", "Imported", "assistant", false});
    SeedLegacy(source);
    const auto result = MigrateLegacyCompanion(source, target,
        [&](auto phase, const auto&)
        {
            if (phase == CompanionMigrationPhase::Publish)
            {
                auto stage = target.Root();
                stage += ".migration";
                Write(stage / "RuntimeData/Preferences/preferences.json", R"({"language":"tampered after validation"})");
            }
            return true;
        });
    Check(!result.complete && !fs::exists(target.Root()), "Migration published an artifact changed after its earlier validation.");
}

void TestMigrationResumesAfterInjectedNoSpace()
{
    ScopedTestDirectory directory;
    CompanionPaths source(directory.root, Legacy), target(directory.root, {"no-space", "Imported", "assistant", false});
    SeedLegacy(source);
    struct SourceArtifact
    {
        fs::path relative;
        std::string bytes;
    };
    std::vector<SourceArtifact> originals;
    for (const auto& relative : InventoryLegacyCompanion(source))
        originals.push_back({relative, Read(source.Resolve(relative))});
    Check(originals.size() > 1, "No-space fixture needs multiple source artifacts.");
    unsigned copyCalls = 0;
    fs::path firstCopied;
    const auto failed = MigrateLegacyCompanion(source, target,
        [&](auto phase, const auto& relative)
        {
            if (phase == CompanionMigrationPhase::Copy)
            {
                if (++copyCalls == 2)
                    throw fs::filesystem_error(
                        "Synthetic staged-copy ENOSPC", source.Resolve(relative), std::make_error_code(std::errc::no_space_on_device));
                firstCopied = relative;
            }
            return true;
        });
    auto stage = target.Root();
    stage += ".migration";
    Check(copyCalls == 2 && failed.copiedArtifacts == 1 && !failed.complete && !failed.interrupted && !failed.error.empty(),
        "Synthetic ENOSPC did not stop after actual staged progress as a storage failure.");
    Check(!fs::exists(target.Root()) && fs::exists(stage / firstCopied) && fs::exists(stage / "migration-manifest.json"),
        "Synthetic ENOSPC activated a partial root or discarded resumable staged progress.");
    for (const auto& original : originals)
        Check(Read(source.Resolve(original.relative)) == original.bytes, "Synthetic ENOSPC changed a source artifact.");
    const auto resumed = MigrateLegacyCompanion(source, target);
    Check(resumed.complete && resumed.copiedArtifacts == originals.size() - 1 && !fs::exists(stage),
        "Migration did not reuse its validated staged artifact and publish safely after synthetic ENOSPC.");
    for (const auto& original : originals)
        Check(Read(source.Resolve(original.relative)) == original.bytes, "Resuming synthetic ENOSPC changed a source artifact.");
    Check(!MigrateLegacyCompanion(source, target).complete, "Repeated startup replaced an already activated root.");
}

void TestMigrationRecoversAnInterruptedTemporaryDatabase()
{
    ScopedTestDirectory directory;
    CompanionPaths source(directory.root, Legacy), target(directory.root, {"temporary", "Imported", "assistant", false});
    SeedLegacy(source);
    const auto interrupted =
        MigrateLegacyCompanion(source, target, [](auto phase, const auto&) { return phase != CompanionMigrationPhase::Copy; });
    Check(interrupted.interrupted, "Temporary-copy fixture did not stage a manifest.");
    auto stage = target.Root();
    stage += ".migration";
    Write(stage / "Goals/revia_goals.db.tmp", "An incomplete interrupted SQLite copy.");
    const auto resumed = MigrateLegacyCompanion(source, target);
    Check(resumed.complete, "Migration could not resume an interrupted temporary database: " + resumed.error);
}

void TestMigrationIncludesEveryStandardPrivateOwner()
{
    ScopedTestDirectory directory;
    CompanionPaths source(directory.root, Legacy), target(directory.root, {"owners", "Imported", "assistant", false});
    SeedLegacy(source);
    const std::vector<fs::path> sentinels{"RuntimeData/Songs/sentinel.txt", "RuntimeData/SpeechInput/sentinel.txt",
        "RuntimeData/Evaluations/sentinel.txt", "RuntimeData/Agents/sentinel.txt", "Audit/sentinel.txt",
        "RuntimeData/Skills/sentinel.txt", "RuntimeData/Learning/sentinel.txt"};
    for (const auto& relative : sentinels)
        Write(source.Resolve(relative), "Private owner sentinel " + relative.generic_string());
    const auto result = MigrateLegacyCompanion(source, target);
    Check(result.complete, result.error);
    for (const auto& relative : sentinels)
        Check(Read(target.Resolve(relative)) == Read(source.Resolve(relative)),
            "Migration omitted a standard private owner: " + relative.generic_string());
}

void TestWholeDocumentJsonValidation()
{
    std::vector<std::string> admitted;
    struct Suffix
    {
        std::string label;
        std::string text;
    };
    const std::vector<Suffix> suffixes{{"trailing garbage", " trailing-garbage"}, {"second document", "\n{}"},
        {"raw NUL", std::string(1, '\0')}, {"NUL and garbage", std::string(1, '\0') + "trailing-garbage"}};
    for (const auto& [label, suffix] : suffixes)
    {
        {
            ScopedTestDirectory directory;
            CompanionRegistry original(directory.root);
            CompanionDescriptor descriptor;
            std::string error;
            Check(original.Load(error) && original.Create("Fixture", "assistant", descriptor, error), error);
            const auto file = directory.root / "RuntimeData/Companions/registry.json";
            const auto corrupt = Read(file) + suffix;
            Write(file, corrupt);
            CompanionRegistry loaded(directory.root);
            if (loaded.Load(error))
                admitted.push_back("registry " + label);
            else
                Check(!loaded.Create("Must not overwrite", "assistant", descriptor, error) && Read(file) == corrupt,
                    "A refused registry was overwritten after whole-document validation.");
        }
        {
            ScopedTestDirectory directory;
            CompanionPaths source(directory.root, Legacy), target(directory.root, {"document", "Imported", "assistant", false});
            SeedLegacy(source);
            const auto file = source.Resolve("RuntimeData/Preferences/preferences.json");
            const auto corrupt = Read(file) + suffix;
            Write(file, corrupt);
            if (MigrateLegacyCompanion(source, target).complete)
                admitted.push_back("artifact " + label);
            else
                Check(!fs::exists(target.Root()) && Read(file) == corrupt,
                    "A refused migration document was published or altered its source.");
        }
        {
            ScopedTestDirectory directory;
            CompanionPaths source(directory.root, Legacy), target(directory.root, {"manifest", "Imported", "assistant", false});
            SeedLegacy(source);
            const auto interrupted =
                MigrateLegacyCompanion(source, target, [](auto phase, const auto&) { return phase != CompanionMigrationPhase::Publish; });
            Check(interrupted.interrupted, "Strict manifest fixture did not reach staged publication.");
            auto stage = target.Root();
            stage += ".migration";
            const auto file = stage / "migration-manifest.json";
            const auto corrupt = Read(file) + suffix;
            Write(file, corrupt);
            if (MigrateLegacyCompanion(source, target).complete)
                admitted.push_back("manifest " + label);
            else
                Check(!fs::exists(target.Root()) && Read(file) == corrupt, "A refused resumed manifest was overwritten or published.");
        }
        if (suffix.front() == '\0')
        {
            ScopedTestDirectory directory;
            CompanionPaths source(directory.root, Legacy), target(directory.root, {"lines", "Imported", "assistant", false});
            const auto file = source.Resolve("Logs/records.jsonl");
            const auto corrupt = std::string("{}") + suffix + '\n';
            Write(file, corrupt);
            if (MigrateLegacyCompanion(source, target).complete)
                admitted.push_back("JSONL " + label);
            else
                Check(!fs::exists(target.Root()) && Read(file) == corrupt, "A refused JSONL record was published or altered its source.");
        }
    }
    {
        ScopedTestDirectory directory;
        CompanionRegistry original(directory.root);
        CompanionDescriptor descriptor;
        std::string error;
        Check(original.Load(error) && original.Create("Whitespace", "assistant", descriptor, error), error);
        const auto registry = directory.root / "RuntimeData/Companions/registry.json";
        Write(registry, Read(registry) + " \r\n\t");
        CompanionRegistry loaded(directory.root);
        Check(loaded.Load(error), "Valid registry trailing whitespace was refused.");
        CompanionPaths source(directory.root, Legacy), target(directory.root, {"whitespace", "Imported", "assistant", false});
        SeedLegacy(source);
        const auto document = source.Resolve("RuntimeData/Preferences/preferences.json");
        Write(document, R"({"language":"\u0000"})"
                        " \r\n\t");
        Check(MigrateLegacyCompanion(source, target, [](auto phase, const auto&) { return phase != CompanionMigrationPhase::Publish; })
                  .interrupted,
            "Valid document trailing whitespace was refused before staging.");
        auto stage = target.Root();
        stage += ".migration";
        const auto manifest = stage / "migration-manifest.json";
        Write(manifest, Read(manifest) + " \r\n\t");
        Check(MigrateLegacyCompanion(source, target).complete, "Valid resumed manifest trailing whitespace was refused.");
        Check(Read(target.Resolve("RuntimeData/Preferences/preferences.json")) == Read(document),
            "A valid escaped Unicode NUL was refused or altered.");
    }
    for (const auto& example : admitted)
        std::cout << "INCORRECTLY ADMITTED " << example << '\n';
    Check(admitted.empty(), "Companion JSON validation admitted a valid prefix with trailing content.");
}
}

void RunCompanionTests()
{
    TestCapturedPathsAndRegistry();
    std::cout << "PASS companion paths/registry\n";
    TestNeutralProfilesAndPrivateSaves();
    std::cout << "PASS private profiles/neutral seed\n";
    TestConcurrentActualOwners();
    std::cout << "PASS concurrent actual memory/prompt/log owners\n";
    TestAllRouterTiersUseCapturedMemory();
    std::cout << "PASS actual Main/Fast/Expert captured memory loopback\n";
    TestStaleQueuedEvaluationAndLearnedFinding();
    std::cout << "PASS captured stale admission and learned commit\n";
    TestStaleBackfillCannotCommitOrPublish();
    std::cout << "PASS real blocked backfill admission/commit/publication\n";
    TestStorageAwareResumableMigration();
    std::cout << "PASS WAL/stable IDs/resume/atomic private migration\n";
    TestMigrationRejectsCorruptNewerAndStorageFailures();
    std::cout << "PASS migration corruption/newer/source change/storage/publication guards\n";
    TestMigrationRecoversAnInterruptedTemporaryDatabase();
    std::cout << "PASS migration incomplete temporary database recovery\n";
    TestMigrationRevalidatesAtPublication();
    std::cout << "PASS migration revalidation at publication\n";
    TestMigrationResumesAfterInjectedNoSpace();
    std::cout << "PASS synthetic staged ENOSPC preserves source/inactive target and resumes safely\n";
    TestMigrationIncludesEveryStandardPrivateOwner();
    std::cout << "PASS migration all standard private owner roots\n";
    TestWholeDocumentJsonValidation();
    std::cout << "PASS strict whole-document registry/artifact/manifest JSON\n";
}
