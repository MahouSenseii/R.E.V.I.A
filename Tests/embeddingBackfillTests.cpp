#include "memoryAgentTestAccess.h"
#include "reviaSessionTestAccess.h"

#include <atomic>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace
{
using namespace std::chrono_literals;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
using Access = revia::runtime::ReviaSessionTestAccess;
using revia::agents::MemoryAgent;
using revia::agents::MemoryAgentTestAccess;
using revia::agents::LearnedFindingResult;
using revia::runtime::ReviaSession;
using json = nlohmann::json;

template<class Predicate>
bool Until(Predicate predicate, std::chrono::milliseconds timeout = 8s)
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    do
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < end);
    return false;
}

memoryDecision Finding(const std::string& tag)
{
    memoryDecision value;
    value.bSuccess = value.bShouldRemember = true;
    value.category = "test";
    value.source = "autonomous_research";
    value.summary = "alpha" + tag + " beta" + tag + " gamma" + tag + " delta" + tag;
    return value;
}

void Seed(const std::string& path, const std::string& tag)
{
    bool added = false;
    Check(longTermMemory(path).Save(Finding(tag), added) && added, "Could not seed a distinct finding.");
}

void ExecuteSql(const std::string& path, const char* sql)
{
    sqlite3* database = nullptr;
    const int opened = sqlite3_open(path.c_str(), &database);
    const int result = opened == SQLITE_OK
        ? sqlite3_exec(database, sql, nullptr, nullptr, nullptr) : opened;
    const std::string error = database ? sqlite3_errmsg(database) : "open failed";
    if (database) sqlite3_close(database);
    Check(result == SQLITE_OK, "Backfill fixture SQL failed: " + error);
}

int Count(const std::string& path, const char* sql)
{
    sqlite3* database = nullptr;
    Check(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "Could not read the backfill fixture.");
    sqlite3_busy_timeout(database, 2000);
    sqlite3_stmt* statement = nullptr;
    const int prepared = sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
    const int status = prepared == SQLITE_OK ? sqlite3_step(statement) : prepared;
    const int result = status == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
    const std::string error = sqlite3_errmsg(database);
    if (statement) sqlite3_finalize(statement);
    sqlite3_close(database);
    Check(result >= 0, "Could not count backfill fixture rows (" + std::to_string(status) + "): " + error);
    return result;
}

class WorkingDirectory
{
public:
    explicit WorkingDirectory(const std::filesystem::path& path)
        : previous(std::filesystem::current_path()) { std::filesystem::current_path(path); }
    ~WorkingDirectory() { std::filesystem::current_path(previous); }
private:
    std::filesystem::path previous;
};

class Backend
{
public:
    Backend()
    {
        main.Get("/health", [](const auto&, auto& response)
        { response.set_content(R"({"status":"ok","slots_idle":1})", "application/json"); });
        main.Get("/v1/models", [](const auto&, auto& response)
        { response.set_content(R"({"data":[{"id":"fixture-main"}]})", "application/json"); });
        main.Get("/props", [](const auto&, auto& response)
        { response.set_content(R"({"total_slots":1,"default_generation_settings":{"n_ctx":8192}})", "application/json"); });
        main.Post("/v1/chat/completions", [this](const auto&, auto& response)
        {
            RecordRequest("turn");
            response.set_content(json{{"choices", json::array({{{"message", {
                {"content", R"({"shouldRemember":false,"reason":"Fixture classification."})"}}}}})}}.dump(),
                "application/json");
        });
        embeddings.Get("/health", [this](const auto&, auto& response)
        {
            ++healthRequests;
            Until([&] { return !blockHealth.load(); }, 10s);
            response.status = healthy ? 200 : 503;
            response.set_content(R"({"status":"ok"})", "application/json");
        });
        embeddings.Get("/v1/models", [this](const auto&, auto& response)
        {
            response.set_content(json{{"data", json::array({{{"id",
                wrongModel ? "other-model" : "fixture-embedding"}}})}}.dump(), "application/json");
        });
        embeddings.Post("/v1/embeddings", [this](const auto& request, auto& response)
        {
            ++requests;
            const auto input = json::parse(request.body).value("input", "");
            RecordRequest(input.find("alphalearning") != std::string::npos ? "learning" : "backfill");
            Until([&] { return !blockEmbedding.load(); }, 10s);
            const bool failed = !healthy || (failFirstRow && input.find("alpharow0 ") != std::string::npos);
            if (failed) { response.status = 503; return; }
            {
                std::lock_guard lock(mutex);
                ++successfulRequests[input];
            }
            response.set_content(R"({"data":[{"embedding":[1.0,0.0,0.0]}]})", "application/json");
        });
        main.new_task_queue = [] { return new httplib::ThreadPool(2); };
        embeddings.new_task_queue = [] { return new httplib::ThreadPool(2); };
        mainPort = main.bind_to_any_port("127.0.0.1");
        port = embeddings.bind_to_any_port("127.0.0.1");
        Check(mainPort > 0 && port > 0, "Could not bind backfill fixture ports.");
        mainThread = std::jthread([this] { main.listen_after_bind(); });
        embeddingThread = std::jthread([this] { embeddings.listen_after_bind(); });
        if (!Until([&] { return main.is_running() && embeddings.is_running(); }))
        {
            main.stop();
            embeddings.stop();
            mainThread.join();
            embeddingThread.join();
            Check(false, "Fixture did not start.");
        }
    }
    ~Backend()
    {
        blockHealth = blockEmbedding = false;
        main.stop();
        embeddings.stop();
        mainThread.join();
        embeddingThread.join();
    }
    void Configure(messageRouter& router)
    {
        llmSettings llm;
        llm.host = "127.0.0.1";
        llm.port = mainPort;
        llm.modelName = "fixture-main";
        llm.bAutoStartServer = false;
        embeddingSettings embedding;
        embedding.host = "127.0.0.1";
        embedding.port = port;
        embedding.modelName = "fixture-embedding";
        embedding.bAutoStartServer = false;
        router.ApplyLLMSettings(llm, embedding, aiProfile{});
    }
    void ConfigureSession(const std::filesystem::path& root)
    {
        std::filesystem::create_directories(root / "Config/Profiles");
        const json settings = {
            {"activeProfile", "fixture"},
            {"llm", {{"backend", "LLamaCpp"}, {"host", "127.0.0.1"}, {"port", mainPort},
                {"modelName", "fixture-main"}, {"autoStartServer", false}, {"visionEnabled", false},
                {"modelPath", (root / "absent.gguf").string()},
                {"mediaPath", (root / "RuntimeData/Vision").string()}}},
            {"embedding", {{"enabled", true}, {"host", "127.0.0.1"}, {"port", port},
                {"modelName", "fixture-embedding"}, {"autoStartServer", false}}},
            {"intelligence", {{"enabled", false}}},
            {"speech", {{"enabled", false}, {"backend", "WindowsSapi"}, {"speakGreeting", false},
                {"voiceDataPath", (root / "RuntimeData/Voices").string()}}},
            {"speechRecognition", {{"enabled", false}}},
            {"presence", {{"enabled", false}, {"externalAdaptersEnabled", false}}},
            {"vision", {{"enabled", false}}}, {"perception", {{"enabled", false}}},
            {"initiative", {{"enabled", false}}}, {"bargeIn", {{"enabled", false}}},
            {"conversation", {{"archiveEnabled", false}}}, {"image", {{"enabled", false}}}
        };
        std::ofstream(root / "Config/settings.json") << settings.dump(2);
        for (const bool enabled : {true, false})
        {
            const auto id = enabled ? "fixture" : "no-memory";
            std::ofstream(root / "Config/Profiles" / (std::string(id) + ".json")) <<
                json{{"id", id}, {"displayName", id}, {"systemPrompt", "Backfill fixture."},
                    {"shouldSpeak", false}, {"memoryEnabled", enabled}}.dump(2);
        }
    }
    void CheckSuccessfulRequestsAreUnique()
    {
        std::lock_guard lock(mutex);
        for (const auto& [input, count] : successfulRequests)
            Check(count == 1, "A successfully indexed document was embedded again.");
    }
    void CheckEmbeddedSummary(const std::string& expected)
    {
        std::lock_guard lock(mutex);
        Check(successfulRequests.size() == 1 &&
            successfulRequests.begin()->first.find(expected) != std::string::npos,
            "Duplicate learning embedded incoming text instead of the retained summary.");
    }
    std::vector<std::string> RequestOrder()
    {
        std::lock_guard lock(mutex);
        return requestOrder;
    }
    std::atomic<bool> healthy{true}, wrongModel{false}, failFirstRow{false};
    std::atomic<bool> blockHealth{false}, blockEmbedding{false};
    std::atomic<int> requests{0}, healthRequests{0};
private:
    void RecordRequest(const std::string& type)
    {
        std::lock_guard lock(mutex);
        requestOrder.push_back(type);
    }
    httplib::Server main, embeddings;
    std::jthread mainThread, embeddingThread;
    int mainPort = 0, port = 0;
    std::mutex mutex;
    std::map<std::string, int> successfulRequests;
    std::vector<std::string> requestOrder;
};

void TestSessionContinuationRecoveryAndProfileGates()
{
    ScopedTestDirectory directory;
    Backend backend;
    backend.ConfigureSession(directory.root);
    WorkingDirectory cwd(directory.root);
    const auto path = (directory.root / "Memory/revia_memory.db").string();
    for (int index = 0; index < 55; ++index) Seed(path, "row" + std::to_string(index));
    ExecuteSql(path,
        "CREATE TABLE vector_writes(memory_id TEXT);"
        "CREATE TRIGGER count_vector_insert AFTER INSERT ON memory_embeddings BEGIN "
        "INSERT INTO vector_writes VALUES (new.memory_id); END;"
        "CREATE TRIGGER count_vector_update AFTER UPDATE ON memory_embeddings BEGIN "
        "INSERT INTO vector_writes VALUES (new.memory_id); END;");
    const auto vectors = [&] { return Count(path, "SELECT count(*) FROM memory_embeddings;"); };
    {
        ReviaSession session;
        MemoryAgentTestAccess::FastBackfill(Access::Memory(session));
        backend.failFirstRow = true;
        Check(session.Start(), "The actual session did not start.");
        Check(Until([&] { return vectors() == 54; }),
            "Session did not continue beyond the first batch, or failed early rows starved later ones.");
        backend.failFirstRow = false;
        Check(Until([&] { return vectors() == 55; }), "A failed row was never retried after recovery.");

        revia::agents::MemoryQueueLimits limits;
        limits.maximumLearning = 0;
        Access::Memory(session).SetQueueLimits(limits);
        backend.healthy = false;
        const int beforeFailure = backend.healthRequests.load();
        Check(Access::SubmitLearning(session, Finding("late")) == LearnedFindingResult::SavedWithoutEmbedding,
            "Could not exercise the real queue-pressure fallback.");
        Check(Until([&] { return backend.healthRequests > beforeFailure; }), "Newly saved rows were not discovered.");
        const int afterFailure = backend.healthRequests.load();
        const int posts = backend.requests.load();
        std::this_thread::sleep_for(250ms);
        Check(backend.healthRequests - afterFailure <= 3 && backend.requests == posts,
            "An unavailable backend was hammered or bypassed readiness validation.");
        backend.healthy = true;
        backend.wrongModel = true;
        std::this_thread::sleep_for(350ms);
        Check(vectors() == 55 && backend.requests == posts, "Backfill accepted an unverified model.");
        backend.wrongModel = false;
        Check(Until([&] { return vectors() == 56; }), "Fallback-saved content did not recover its vector.");

        const auto expectQuiet = [&](const std::string& tag)
        {
            const int before = backend.requests.load();
            Seed(path, tag);
            std::this_thread::sleep_for(300ms);
            Check(backend.requests == before, "A disabled profile kept starting embedding work.");
        };
        Check(session.ActivateProfile("no-memory").succeeded, "Could not disable memory through profile activation.");
        expectQuiet("activation");
        Check(session.ActivateProfile("fixture").succeeded, "Could not restore the memory profile.");
        Check(Until([&] { return vectors() == 57; }), "Profile activation did not resume backfill.");

        revia::runtime::ProfileSummary edited;
        edited.id = "fixture";
        edited.displayName = "fixture";
        edited.systemPrompt = "Backfill fixture.";
        edited.memoryEnabled = false;
        Check(session.SaveProfile(edited).succeeded, "Active profile save failed.");
        expectQuiet("profileedit");
        edited.memoryEnabled = true;
        Check(session.SaveProfile(edited).succeeded, "Active profile re-enable failed.");
        Check(Until([&] { return vectors() == 58; }), "Active profile save did not resume backfill.");

        Check(session.Submit("/profile no-memory").succeeded, "CLI profile disable failed.");
        expectQuiet("command");
        Check(session.Submit("/profile fixture").succeeded, "CLI profile re-enable failed.");
        Check(Until([&] { return vectors() == 59; }), "CLI profile change did not resume backfill.");
        std::this_thread::sleep_for(300ms);
        Check(Count(path, "SELECT count(*) FROM vector_writes;") == 59,
            "A periodic rescan saved the same vector more than once.");
        backend.CheckSuccessfulRequestsAreUnique();

        const int beforeBlocked = backend.requests.load();
        backend.blockEmbedding = true;
        Seed(path, "profile-cancel");
        Check(Until([&] { return backend.requests > beforeBlocked; }),
            "Profile cancellation fixture did not reach inference.");
        Check(session.ActivateProfile("no-memory").succeeded, "Could not disable an in-flight backfill.");
        Access::SubmitMemoryEvaluation(session, "Hello", 999);
        const bool released = Until([&]
        {
            const auto events = Access::Memory(session).DrainEvents();
            return std::any_of(events.begin(), events.end(), [](const auto& event)
            { return event.turnId == 999 && event.decision.bSuccess; });
        }, 2s);
        backend.blockEmbedding = false;
        Check(released && vectors() == 59,
            "Session profile disable did not cancel the request and release the actual worker.");
        Check(session.ActivateProfile("fixture").succeeded, "Could not resume the cancelled profile row.");
        Check(Until([&] { return vectors() == 60; }), "Cancelled profile content never recovered its vector.");
        Check(Count(path, "SELECT count(*) FROM vector_writes;") == 60,
            "Profile cancellation saved a vector twice.");
        session.Stop();
    }
    // Readiness at startup is not permission to permanently abandon an enabled scan.
    backend.healthy = false;
    Seed(path, "offline-start");
    {
        ReviaSession restarted;
        MemoryAgentTestAccess::FastBackfill(Access::Memory(restarted));
        Check(restarted.Start(), "Offline-embedding session could not start.");
        backend.healthy = true;
        Check(Until([&] { return vectors() == 61; }), "Startup outage left no production recovery connection.");
        restarted.Stop();
    }
}

void TestCancellationAndInflightDeduplication()
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "memory.db").string();
    Seed(path, "row0");
    Seed(path, "row1");
    Backend backend;
    messageRouter router;
    backend.Configure(router);
    MemoryAgent agent(path);
    MemoryAgentTestAccess::FastBackfill(agent);
    backend.blockHealth = true;
    agent.StartEmbeddingBackfill(router, "fixture-embedding");
    Check(Until([&] { return backend.healthRequests > 0; }), "Backfill health request did not start.");
    auto started = std::chrono::steady_clock::now();
    agent.StopEmbeddingBackfill();
    agent.Submit(router, "Hello", "", 501);
    const bool workerReleased = Until([&]
    {
        const auto events = agent.DrainEvents();
        return std::any_of(events.begin(), events.end(), [](const auto& event)
        { return event.turnId == 501 && event.decision.bSuccess; });
    }, 2s);
    backend.blockHealth = false;
    Check(workerReleased && std::chrono::steady_clock::now() - started < 2s,
        "Disabling backfill left its health request blocking the memory worker.");
    backend.blockEmbedding = true;
    agent.StartEmbeddingBackfill(router, "fixture-embedding");
    Check(Until([&] { return backend.requests == 1; }), "The restarted backfill did not reach inference.");
    Check(agent.SubmitLearnedFinding(router, Finding("row0")) == LearnedFindingResult::AlreadyExists,
        "An in-flight backfill admitted duplicate learning vector work.");
    agent.SubmitEmbeddingBackfill(router, "fixture-embedding");
    agent.SubmitEmbeddingBackfill(router, "fixture-embedding");
    Check(agent.Depths().backfill <= 1, "Repeated scans duplicated queued or in-flight rows.");
    started = std::chrono::steady_clock::now();
    agent.Stop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    backend.blockEmbedding = false;
    Check(elapsed < 2000 && backend.requests == 1, "Stop waited for inference or began another backfill request.");
    Check(Count(path, "SELECT count(*) FROM memory_embeddings;") == 0,
        "Cancelled backfill wrote a vector after Stop.");
    std::cout << "Backfill Stop during blocked inference: " << elapsed << "ms\n";
}

void TestBackfillStorageErrorsAreExplicit()
{
    ScopedTestDirectory directory;
    Check(!longTermMemory(directory.root.string()).ScanMissingEmbeddings("fixture", 0).error.empty(),
        "An unavailable database looked like a completed empty scan.");
}

void TestActualWorkerPriorityAndProgress()
{
    ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    const auto path = (directory.root / "Memory/revia_memory.db").string();
    for (int index = 0; index < 10; ++index) Seed(path, "priority" + std::to_string(index));
    Backend backend;
    messageRouter router;
    backend.Configure(router);
    MemoryAgent agent(path);
    MemoryAgentTestAccess::FastBackfill(agent);
    backend.blockEmbedding = true;
    agent.StartEmbeddingBackfill(router, "fixture-embedding");
    Check(Until([&] { return backend.requests == 1; }), "Priority fixture did not reach backfill.");
    for (int index = 0; index < 16; ++index)
        agent.Submit(router, "I maintain a long-term astronomy project number " + std::to_string(index), "", index + 1);
    for (int index = 0; index < 8; ++index)
        Check(agent.SubmitLearnedFinding(router, Finding("learning" + std::to_string(index))) ==
            LearnedFindingResult::SavedEmbeddingQueued, "Priority fixture could not queue learning.");
    backend.blockEmbedding = false;
    Check(Until([&] { return backend.RequestOrder().size() == 34; }),
        "The actual worker starved a class or repeated completed vector work.");
    const auto order = backend.RequestOrder();
    const auto first = order.begin() + 1;
    Check(std::count(first, first + 7, "turn") == 4 &&
        std::count(first, first + 7, "learning") == 2 &&
        std::count(first, first + 7, "backfill") == 1,
        "The worker did not preserve user-turn priority and progress for both background classes.");
    Check(Until([&] { return Count(path, "SELECT count(*) FROM memory_embeddings;") == 18; }),
        "The worker did not finish the mixed backlog.");
    backend.CheckSuccessfulRequestsAreUnique();
    agent.Stop();
}
}

void TestDuplicateLearningEmbedsRetainedSummary()
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "memory.db").string();
    auto original = Finding("duplicate");
    original.summary = "The user likes coffee.";
    bool added = false;
    longTermMemory store(path);
    Check(store.Save(original, added) && added, "Could not seed unindexed duplicate.");
    Backend backend;
    messageRouter router;
    backend.Configure(router);
    MemoryAgent agent(path);
    auto duplicate = original;
    duplicate.summary = "  THE user\tlikes  coffee.\n";
    Check(agent.SubmitLearnedFinding(router, duplicate) == LearnedFindingResult::SavedEmbeddingQueued,
        "Unindexed duplicate did not queue optional embedding.");
    Check(Until([&] { return store.LoadMissingEmbeddings("fixture-embedding").empty(); }),
        "Retained summary was not indexed.");
    agent.Stop();
    backend.CheckEmbeddedSummary(original.summary);
    Check(store.Load().size() == 1 && store.Load().front().summary == original.summary,
        "Duplicate learning changed the accepted content.");
}

void RunEmbeddingBackfillTests()
{
    TestDuplicateLearningEmbedsRetainedSummary();
    TestSessionContinuationRecoveryAndProfileGates();
    TestCancellationAndInflightDeduplication();
    TestBackfillStorageErrorsAreExplicit();
    TestActualWorkerPriorityAndProgress();
    std::cout << "Continuous embedding backfill owner tests passed.\n";
}
