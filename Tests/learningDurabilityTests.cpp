#include "reviaSessionTestAccess.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <sqlite3.h>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using revia::agents::LearnedFindingResult;
using revia::agents::MemoryAgent;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;

template<class Predicate>
bool Until(Predicate predicate, std::chrono::milliseconds timeout = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

memoryDecision Approved(const std::string& tag)
{
    memoryDecision decision;
    decision.bSuccess = true;
    decision.bShouldRemember = true;
    decision.category = "test";
    decision.source = "autonomous_research";
    decision.summary = "alpha" + tag + " beta" + tag + " gamma" + tag + " delta" + tag;
    decision.reason = "approved learning fixture";
    return decision;
}

class EmbeddingFixture
{
public:
    EmbeddingFixture()
    {
        server.Post("/v1/embeddings", [this](const httplib::Request&, httplib::Response& response)
        {
            ++requests;
            Until([&] { return released.load(); }, 10s);
            response.set_content(R"({"data":[{"embedding":[1.0,0.0,0.0]}]})", "application/json");
        });
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind the local embedding fixture.");
        listener = std::jthread([this] { server.listen_after_bind(); });
        if (!Until([&] { return server.is_running(); }))
        {
            server.stop();
            listener.join();
            Check(false, "Embedding fixture did not start.");
        }
    }
    ~EmbeddingFixture()
    {
        released.store(true);
        server.stop();
        if (listener.joinable()) listener.join();
    }
    void Configure(messageRouter& router) const
    {
        llmSettings llm;
        llm.host = "127.0.0.1";
        llm.port = port;
        llm.bAutoStartServer = false;
        embeddingSettings embeddings;
        embeddings.host = "127.0.0.1";
        embeddings.port = port;
        embeddings.modelName = "fixture-embedding";
        embeddings.bAutoStartServer = false;
        aiProfile profile;
        router.ApplyLLMSettings(llm, embeddings, profile);
    }
    std::atomic<int> requests = 0;
    std::atomic<bool> released = false;
private:
    int port = 0;
    httplib::Server server;
    std::jthread listener;
};

void CheckExactlyOnce(const std::string& path, const std::vector<memoryDecision>& findings)
{
    const auto entries = longTermMemory(path).Load();
    Check(entries.size() == findings.size(), "Accepted findings were lost, duplicated, or mixed with unclassified input.");
    for (const auto& finding : findings)
        Check(std::count_if(entries.begin(), entries.end(), [&](const auto& entry)
            { return entry.summary == finding.summary; }) == 1, "A finding did not survive exactly once.");
}

void TestContentPrecedesEmbeddingAndSurvivesStop()
{
    ScopedTestDirectory temporary;
    const auto path = (temporary.root / "memory.db").string();
    EmbeddingFixture fixture;
    messageRouter router;
    fixture.Configure(router);
    MemoryAgent agent(path);
    const std::vector findings{Approved("one"), Approved("two"), Approved("three")};
    Check(agent.SubmitLearnedFinding(router, findings[0]) == LearnedFindingResult::SavedEmbeddingQueued,
        "Accepted content was not reported saved with embedding queued.");
    Check(Until([&] { return fixture.requests.load() == 1; }), "The real embedding request did not begin.");
    for (std::size_t i = 1; i < findings.size(); ++i)
        Check(agent.SubmitLearnedFinding(router, findings[i]) == LearnedFindingResult::SavedEmbeddingQueued,
            "A queued approved finding was not accepted.");
    Check(agent.Depths().learning == 2, "Fixture did not hold two queued findings behind in-flight embedding.");
    // Before the service has returned, and before Stop can attempt any rescue.
    CheckExactlyOnce(path, findings);
    agent.Submit(router, "Remember this unclassified candidate.");
    memoryDecision unapproved;
    unapproved.summary = "Unapproved candidate must not be stored.";
    Check(agent.SubmitLearnedFinding(router, unapproved) == LearnedFindingResult::Failed,
        "An unapproved candidate was accepted by the persistence boundary.");
    const auto started = std::chrono::steady_clock::now();
    agent.Stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    fixture.released.store(true);
    std::cout << "Blocked embedding Stop: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << "ms\n";
    Check(elapsed < 2s, "Stop waited for the blocked embedding service instead of cancelling it.");
    Check(fixture.requests.load() == 1, "Shutdown began new embedding requests for queued findings.");
    CheckExactlyOnce(path, findings);
    Check(agent.SubmitLearnedFinding(router, Approved("stopped")) == LearnedFindingResult::Failed,
        "A stopped memory owner accepted new work.");

    MemoryAgent restarted(path);
    revia::agents::MemoryQueueLimits limits;
    limits.maximumLearning = 0;
    restarted.SetQueueLimits(limits);
    for (const auto& finding : findings)
        Check(restarted.SubmitLearnedFinding(router, finding) == LearnedFindingResult::AlreadyExists,
            "Restart did not recognize an already committed finding.");
    restarted.Stop();
    CheckExactlyOnce(path, findings);
}

void TestOptionalVectorUpdatesTheAcceptedRow()
{
    ScopedTestDirectory temporary;
    const auto path = (temporary.root / "memory.db").string();
    EmbeddingFixture fixture;
    messageRouter router;
    fixture.Configure(router);
    MemoryAgent agent(path);
    const auto finding = Approved("vector");
    Check(agent.SubmitLearnedFinding(router, finding, 18) == LearnedFindingResult::SavedEmbeddingQueued,
        "Could not accept the vector fixture.");
    CheckExactlyOnce(path, {finding});
    // A slow successful response must retain the original ten-second budget.
    Check(Until([&] { return fixture.requests.load() == 1; }), "Delayed embedding request did not begin.");
    std::this_thread::sleep_for(2200ms);
    fixture.released.store(true);
    bool completed = false;
    Check(Until([&]
    {
        for (const auto& event : agent.DrainEvents())
            if (event.operation == "autonomous_learning")
            {
                Check(event.saveSucceeded && event.wasAdded && event.embeddingError.empty() && event.turnId == 18,
                    "Learning completion misreported previously committed content.");
                completed = true;
            }
        return completed;
    }), "Learning completion did not arrive.");
    agent.Stop();
    CheckExactlyOnce(path, {finding});
    Check(longTermMemory(path).LoadMissingEmbeddings("fixture-embedding").empty(),
        "The optional vector did not update the accepted row.");
}

void TestStorageFailureIsRejectedBeforeQueueing()
{
    ScopedTestDirectory temporary;
    messageRouter router;
    MemoryAgent agent(temporary.root.string()); // A directory cannot be a SQLite database.
    Check(agent.SubmitLearnedFinding(router, Approved("failed")) == LearnedFindingResult::Failed,
        "Unstored content was reported accepted.");
    Check(agent.Depths().learning == 0, "A failed content save still queued embedding work.");
    agent.Stop();
}

class WorkingDirectory
{
public:
    explicit WorkingDirectory(const std::filesystem::path& root)
        : previous(std::filesystem::current_path())
    {
        std::filesystem::create_directories(root / "Config");
        std::filesystem::current_path(root);
    }
    ~WorkingDirectory() { std::filesystem::current_path(previous); }
private:
    std::filesystem::path previous;
};

void TestSessionReportsContentAndVectorFailureSeparately()
{
    ScopedTestDirectory temporary;
    WorkingDirectory cwd(temporary.root);
    const auto path = (temporary.root / "Memory/revia_memory.db").string();
    EmbeddingFixture fixture;
    messageRouter router;
    fixture.Configure(router);
    revia::runtime::ReviaSession session;
    bool sawSaved = false;
    bool sawVectorFailure = false;
    session.Events().Subscribe([&](const revia::runtime::RuntimeEvent& event)
    {
        if (event.component == "Memory" && event.phase == "Saved") sawSaved = true;
        if (event.component == "Embeddings" && event.phase == "Error" &&
            event.message.find("finding is saved") != std::string::npos) sawVectorFailure = true;
    });
    const auto finding = Approved("optionalerror");
    Check(revia::runtime::ReviaSessionTestAccess::SubmitLearning(session, router, finding) ==
        LearnedFindingResult::SavedEmbeddingQueued, "Session coordinator did not accept the finding.");
    Check(Until([&] { return fixture.requests.load() == 1; }), "Session embedding did not reach the fixture.");
    sqlite3* database = nullptr;
    Check(sqlite3_open(path.c_str(), &database) == SQLITE_OK, "Could not open the vector fault fixture.");
    const int fault = sqlite3_exec(database,
        "CREATE TRIGGER reject_test_vector BEFORE INSERT ON memory_embeddings "
        "BEGIN SELECT RAISE(FAIL, 'fixture vector write failure'); END;", nullptr, nullptr, nullptr);
    sqlite3_close(database);
    Check(fault == SQLITE_OK, "Could not install the vector write fault.");
    fixture.released.store(true);
    Check(Until([&]
    {
        session.PollBackgroundEvents();
        return sawSaved && sawVectorFailure;
    }), "Session lost the distinction between committed content and failed vector update.");
    CheckExactlyOnce(path, {finding});
}
}

void RunLearningDurabilityTests()
{
    TestContentPrecedesEmbeddingAndSurvivesStop();
    TestOptionalVectorUpdatesTheAcceptedRow();
    TestStorageFailureIsRejectedBeforeQueueing();
    TestSessionReportsContentAndVectorFailureSeparately();
    std::cout << "Accepted learning durability owner tests passed.\n";
}
