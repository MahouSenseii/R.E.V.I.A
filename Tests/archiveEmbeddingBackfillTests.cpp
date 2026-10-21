#include "testSupport.h"
#include "memoryAgentTestAccess.h"
#include "Core/profile.h"
#include "Memory/longTermMemory.h"

#include <algorithm>
#include <atomic>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace
{
using namespace std::chrono_literals;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
using revia::agents::MemoryAgent;
using revia::agents::MemoryAgentTestAccess;
using namespace revia::memory;

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

MemoryScope Scope(const std::string& participant = "alice")
{
    MemoryScope scope;
    scope.companionId = "fixture-companion";
    scope.participantId = participant;
    scope.audience = {revia::identity::AudienceKind::Private, "private-room", 1, {participant}};
    scope.participantSource = revia::identity::SpeakerSource::ExplicitIntroduction;
    return scope;
}

class EmbeddingBackend
{
public:
    EmbeddingBackend()
    {
        server.Get("/health", [this](const auto&, auto& response)
        {
            ++healthRequests;
            response.set_content(R"({"status":"ok"})", "application/json");
        });
        server.Get("/v1/models", [](const auto&, auto& response)
        {
            response.set_content(R"({"data":[{"id":"archive-fixture-model"}]})", "application/json");
        });
        server.Post("/v1/embeddings", [this](const auto& request, auto& response)
        {
            const auto text = nlohmann::json::parse(request.body).value("input", "");
            {
                std::lock_guard lock(mutex);
                inputs.push_back(text);
            }
            ++requests;
            Until([&]() { return !blocked.load(); }, 10s);
            if (failFirst && text.find("archive-row-0 ") != std::string::npos)
            {
                response.status = 503;
                return;
            }
            response.set_content(R"({"data":[{"embedding":[1.0,0.0,0.0]}]})", "application/json");
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Archive embedding fixture could not bind.");
        worker = std::jthread([this]() { server.listen_after_bind(); });
        if (!Until([&]() { return server.is_running(); }))
        {
            server.stop();
            worker.join();
            Check(false, "Archive embedding fixture did not start.");
        }
    }
    ~EmbeddingBackend()
    {
        blocked = false;
        server.stop();
        worker.join();
    }
    void Configure(messageRouter& router)
    {
        llmSettings llm;
        llm.bAutoStartServer = false;
        llm.port = port;
        llm.modelName = "unused-main";
        embeddingSettings settings;
        settings.host = "127.0.0.1";
        settings.port = port;
        settings.modelName = "archive-fixture-model";
        settings.bAutoStartServer = false;
        router.ApplyLLMSettings(llm, settings, aiProfile{});
    }
    std::vector<std::string> Inputs()
    {
        std::lock_guard lock(mutex);
        return inputs;
    }
    std::atomic<bool> blocked{false}, failFirst{false};
    std::atomic<int> requests{0}, healthRequests{0};
private:
    httplib::Server server;
    std::jthread worker;
    std::mutex mutex;
    std::vector<std::string> inputs;
    int port = 0;
};

void Add(ConversationArchive& archive, const std::string& text, const MemoryScope& scope, const std::string& session = "archive")
{
    std::string error;
    Check(archive.BeginSession(session, error) && archive.Record(session, "user", text, error, scope),
        "Archive backfill could not seed a turn: " + error);
}

void TestArchiveBackfillPagesRecoversAndRescans()
{
    ScopedTestDirectory directory;
    const auto scope = Scope();
    ConversationArchive archive((directory.root / "archive.db").string());
    for (int index = 0; index < 29; ++index)
        Add(archive, "archive-row-" + std::to_string(index) + " described a different pottery glaze.", scope);
    Add(archive, "Bob private sentinel must never be embedded.", Scope("bob"), "bob");
    EmbeddingBackend backend;
    backend.failFirst = true;
    messageRouter router;
    backend.Configure(router);
    MemoryAgent agent((directory.root / "memories.db").string());
    MemoryAgentTestAccess::FastBackfill(agent);
    agent.StartArchiveEmbeddingBackfill(router, archive, "archive-fixture-model", scope);
    Check(Until([&]()
        { return archive.ScanMissingEmbeddings("archive-fixture-model", 0, 100, &scope).entries.size() == 1; }),
        "A failed early archive embedding blocked later pages.");
    Check(archive.NeedsEmbedding("archive:0", "archive-fixture-model"), "Failed archive row unexpectedly acquired a vector.");
    backend.failFirst = false;
    Check(Until([&]() { return !archive.NeedsEmbedding("archive:0", "archive-fixture-model"); }),
        "Archive backfill did not retry an earlier failed row.");
    Add(archive, "A later newly archived reply about a kiln.", scope);
    Check(Until([&]() { return !archive.NeedsEmbedding("archive:29", "archive-fixture-model"); }),
        "The ongoing archive subscription did not discover a newly recorded turn.");
    agent.Stop();
    const auto inputs = backend.Inputs();
    Check(std::none_of(inputs.begin(), inputs.end(), [](const auto& text) { return text.find("Bob private sentinel") != std::string::npos; }),
        "Archive embedding backfill sent another participant's text.");
    Check(std::all_of(inputs.begin(), inputs.end(), [](const auto& text)
        { return text.find("user: ") != std::string::npos; }), "Archive backfill did not use the shared speaker/content representation.");
    Check(archive.NeedsEmbedding("bob:0", "archive-fixture-model"), "Archive backfill persisted a vector outside captured scope.");
}

void TestArchiveBackfillSharesQueueAndStopsCleanly()
{
    ScopedTestDirectory directory;
    const auto memoryPath = (directory.root / "memories.db").string();
    const auto scope = Scope();
    ConversationArchive archive((directory.root / "archive.db").string());
    Add(archive, "archive-row-0 this turn waits during cancellation.", scope);
    Add(archive, "A second archive turn remains queued.", scope);
    longTermMemory store(memoryPath);
    memoryDecision decision;
    decision.bSuccess = decision.bShouldRemember = true;
    decision.category = "fact";
    decision.summary = "A curated fixture fact about an unrelated copper lamp.";
    bool added = false;
    std::string memoryId;
    Check(store.Save(decision, added, &memoryId) && added, "Curated backfill fixture seed failed.");
    sqlite3* database = nullptr;
    Check(sqlite3_open(memoryPath.c_str(), &database) == SQLITE_OK, "Collision fixture database did not open.");
    const int changed = sqlite3_exec(database, "UPDATE memories SET id='archive:0';", nullptr, nullptr, nullptr);
    sqlite3_close(database);
    Check(changed == SQLITE_OK, "Could not seed a cross-store ID collision.");
    memoryId = "archive:0";
    EmbeddingBackend backend;
    backend.blocked = true;
    messageRouter router;
    backend.Configure(router);
    MemoryAgent agent(memoryPath);
    MemoryAgentTestAccess::FastBackfill(agent);
    agent.StartArchiveEmbeddingBackfill(router, archive, "archive-fixture-model", scope);
    Check(Until([&]() { return backend.requests > 0; }), "Archive embedding did not enter the existing worker.");
    // Starting curated recovery must leave the queued archive subscription intact.
    agent.StartEmbeddingBackfill(router, "archive-fixture-model");
    agent.SubmitEmbeddingBackfill(router, "archive-fixture-model");
    Check(agent.Depths().backfill == 2, "A curated memory ID collided with an in-flight archive turn ID.");
    backend.blocked = false;
    Check(Until([&]() { return !store.NeedsEmbedding(memoryId, "archive-fixture-model") &&
        archive.ScanMissingEmbeddings("archive-fixture-model", 0, 25, &scope).entries.empty(); }),
        "Starting curated backfill discarded archive work or starved one store.");
    backend.blocked = true;
    const int previous = backend.requests;
    Add(archive, "A new archive turn is held until stop.", scope);
    Check(Until([&]() { return backend.requests > previous; }), "New archive work did not reach the worker.");
    const auto started = std::chrono::steady_clock::now();
    agent.StopEmbeddingBackfill();
    agent.Stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    Check(elapsed < 2s, "Stopping archive backfill waited for an in-flight network request.");
    backend.blocked = false;
    Check(archive.NeedsEmbedding("archive:2", "archive-fixture-model"), "Cancelled archive backfill committed a late vector.");
    Check(agent.Depths().backfill == 0, "Stopped archive work left pending queue entries.");
}

void TestArchiveBackfillRevokesAdmissionAndRejectsReusedTurn()
{
    ScopedTestDirectory directory;
    const auto scope = Scope();
    ConversationArchive archive((directory.root / "archive.db").string());
    const std::string original = "The original archive turn describes orange pottery.";
    Add(archive, original, scope);
    EmbeddingBackend backend;
    backend.blocked = true;
    messageRouter router;
    backend.Configure(router);
    MemoryAgent agent((directory.root / "memories.db").string());
    MemoryAgentTestAccess::FastBackfill(agent);
    std::atomic<bool> admitted{true};
    agent.StartArchiveEmbeddingBackfill(router, archive, "archive-fixture-model", scope, [&]() { return admitted.load(); });
    Check(Until([&]() { return backend.requests > 0; }), "Admission fixture did not begin archive embedding.");
    admitted = false;
    backend.blocked = false;
    Check(Until([&]() { return agent.Depths().backfill == 0; }), "Revoked archive task did not leave its queue.");
    agent.Stop();
    Check(archive.NeedsEmbedding("archive:0", "archive-fixture-model") && agent.DrainEvents().empty(),
        "Revoked archive admission persisted or published a late result.");

    backend.blocked = true;
    const int previous = backend.requests;
    // The second router avoids the embedding service's completed-query cache.
    messageRouter replacementRouter;
    backend.Configure(replacementRouter);
    MemoryAgent replacementAgent((directory.root / "replacement-memories.db").string());
    MemoryAgentTestAccess::FastBackfill(replacementAgent);
    replacementAgent.StartArchiveEmbeddingBackfill(replacementRouter, archive, "archive-fixture-model", scope);
    Check(Until([&]() { return backend.requests > previous; }), "Replacement fixture did not begin embedding.");
    Check(archive.RemoveLastTurnIf("archive", "user", original), "Replacement fixture failed to retract the original turn.");
    Add(archive, "The regenerated archive turn describes violet pottery.", scope);
    backend.blocked = false;
    Check(Until([&]()
    {
        for (const auto& event : replacementAgent.DrainEvents())
            if (event.operation == "archive_backfill" && !event.saveSucceeded) return true;
        return false;
    }), "A stale vector was not rejected after its archived turn ID was reused.");
    replacementAgent.Stop();
}
}

void RunArchiveEmbeddingBackfillTests()
{
    TestArchiveBackfillPagesRecoversAndRescans();
    TestArchiveBackfillSharesQueueAndStopsCleanly();
    TestArchiveBackfillRevokesAdmissionAndRejectsReusedTurn();
}
