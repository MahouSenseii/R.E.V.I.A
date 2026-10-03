#include "Agents/memoryAgent.h"
#include "Memory/longTermMemory.h"
#include "testSupport.h"
#include "reviaSessionTestAccess.h"

#include <fstream>
#include <iostream>
#include <atomic>
#include <chrono>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <thread>

namespace
{
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;

class HeldMemoryBackend
{
  public:
    HeldMemoryBackend()
    {
        server.Get("/health", [](const auto&, auto& response) { response.set_content(R"({"status":"ok"})", "application/json"); });
        server.Get("/v1/models", [](const auto&, auto& response) { response.set_content(R"({"data":[{"id":"held-memory"}]})", "application/json"); });
        server.Post("/v1/chat/completions", [this](const auto&, auto& response)
        {
            ++requests;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!release.load() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            const auto answer = nlohmann::json{{"choices", nlohmann::json::array({{
                {"message", {{"content", R"({"shouldRemember":false,"reason":"Synthetic fixture."})"}}}, {"finish_reason", "stop"}}})}};
            response.set_content(answer.dump(), "application/json");
        });
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Held memory backend could not bind.");
        worker = std::jthread([this] { server.listen_after_bind(); });
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!server.is_running() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Check(server.is_running(), "Held memory backend could not start.");
    }
    ~HeldMemoryBackend() { release.store(true); server.stop(); if (worker.joinable()) worker.join(); }
    int port = 0;
    std::atomic<unsigned> requests{0};
    std::atomic<bool> release{false};

  private:
    httplib::Server server;
    std::jthread worker;
};

void TestQueuedMemoryRetainsAdmission()
{
    ScopedTestDirectory temporary;
    HeldMemoryBackend backend;
    messageRouter router((temporary.root / "memory.db").string());
    llmSettings settings;
    settings.host = "127.0.0.1";
    settings.port = backend.port;
    settings.modelName = "held-memory";
    settings.bAutoStartServer = settings.bVisionEnabled = false;
    embeddingSettings embedding;
    embedding.bEnabled = false;
    aiProfile profile;
    profile.bMemoryEnabled = true;
    router.ApplyLLMSettings(settings, embedding, profile);
    revia::agents::MemoryAgent memory((temporary.root / "memory.db").string());
    memory.Submit(router, "The synthetic fixture prefers amber.", {}, revia::agents::ResponseProvenance::NormalGeneration, 1);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (backend.requests.load() == 0 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Check(backend.requests.load() == 1, "The first real memory classification did not enter the held transport.");
    std::atomic<bool> current{true};
    std::atomic<bool> denied{false};
    memory.Submit(router, "PRIVATE_QUEUED_SENTINEL_9f32", {}, revia::agents::ResponseProvenance::NormalGeneration, 2,
        [&] { if (!current.load()) denied.store(true); return current.load(); });
    Check(memory.Depths().interactive == 1, "The context-bound second classification was not actually queued.");
    current.store(false);
    backend.release.store(true);
    while (!denied.load() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Check(denied.load() && backend.requests.load() == 1 && longTermMemory((temporary.root / "memory.db").string()).Load().empty(),
        "Revoked queued memory reached transport or durable storage.");
    memory.Stop();
}

void TestAudienceContextAndTaskRegistration()
{
    using namespace revia::runtime;
    ScopedTestDirectory temporary;
    ReviaSession session(CompanionPaths(temporary.root, {"context-fixture", "Context fixture", "assistant", false}));
    ReviaSessionTestAccess::MarkStudioStarted(session, true);
    const auto typed = ReviaSessionTestAccess::StudioInput(session, revia::agents::InputSource::Typed);
    Check(ReviaSessionTestAccess::StudioInputCurrent(session, typed), "A fresh private typed context is not current.");
    auto task = typed.stamp;
    task.taskId = "owned-task";
    task.attemptId = "owned-attempt";
    Check(ReviaSessionTestAccess::RegisterStudioTask(session, task), "The task registration fixture did not execute.");
    Check(ReviaSessionTestAccess::StudioInputCurrent(session, typed),
        "This turn's own task registration was mistaken for an audience change and discarded its command response.");
    auto& people = ReviaSessionTestAccess::People(session);
    (void)people.Get("person-b");
    std::string error;
    Check(people.GrantRecognitionConsent("person-b", error), error);
    const auto revision = people.RecognitionConsentRevision("person-b");
    Check(revision.has_value(), "Recognition consent was not established.");
    Check(session.SetAudience({revia::identity::AudienceKind::Private, "explicit-room", 0, {"person-b"}}, error), error);
    revia::identity::SpeakerObservation observed{"person-b", revia::identity::SpeakerSource::ConsentedVoice,
        "synthetic-observation", *revision, 0.9F};
    const auto voice = ReviaSessionTestAccess::StudioInput(session, revia::agents::InputSource::Voice, observed);
    Check(voice.participantId == "person-b" && voice.audience.kind == revia::identity::AudienceKind::Shared &&
        ReviaSessionTestAccess::StudioInputCurrent(session, voice),
        "A consented match lost its participant or unlocked private owner context.");
    Check(people.RevokeRecognitionConsent("person-b", error), error);
    Check(!ReviaSessionTestAccess::StudioInputCurrent(session, voice), "A revoked consent still admitted its captured voice context.");
    const auto stale = ReviaSessionTestAccess::StudioInput(session, revia::agents::InputSource::Voice, observed);
    Check(stale.audience.kind == revia::identity::AudienceKind::Unknown && stale.participantId != "person-b",
        "Stale recognition retained identifying or private context.");
    ReviaSessionTestAccess::MarkStudioStarted(session, false);
}

void TestLateTaskKeepsItsLaunchAudience()
{
    using namespace revia::runtime;
    ScopedTestDirectory temporary;
    std::atomic<unsigned> delivered{0};
    std::atomic<bool> release{false};
    ReviaSession session(CompanionPaths(temporary.root, {"late-task", "Late task fixture", "assistant", false}));
    ReviaSessionTestAccess::MarkStudioStarted(session, true);
    const auto subscription = session.Events().Subscribe([&](const RuntimeEvent& event)
    {
        if (event.kind == RuntimeEventKind::AssistantMessage && event.component == "Task")
            delivered.fetch_add(1);
    });
    std::string error;
    Check(ReviaSessionTestAccess::LaunchTask(session, "PRIVATE_LATE_TASK_SENTINEL", [&](std::stop_token stop)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!release.load() && !stop.stop_requested() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        revia::goals::Goal goal;
        goal.title = "PRIVATE_LATE_TASK_SENTINEL";
        goal.status = revia::goals::GoalStatus::Succeeded;
        return goal;
    }, error), error);
    Check(session.SetAudience({revia::identity::AudienceKind::Public, "public", 0, {"visitor"}}, error), error);
    Check(session.SetAudience({revia::identity::AudienceKind::Private, "local-desktop", 0, {}}, error), error);
    release.store(true);
    ReviaSessionTestAccess::WaitForTask(session);
    Check(delivered.load() == 0 && !session.HasRunningTask() &&
        ReviaSessionTestAccess::FinishedTask(session).find("PRIVATE_LATE_TASK_SENTINEL") != std::string::npos,
        "Late task report was republished into a new audience epoch or its private receipt was discarded.");
    session.Events().Unsubscribe(subscription);
    ReviaSessionTestAccess::MarkStudioStarted(session, false);
}

void TestTaskObserverCannotChangeReportAudience()
{
    using namespace revia::runtime;
    ScopedTestDirectory temporary;
    std::atomic<bool> release{false};
    std::atomic<bool> audienceChanged{false};
    std::atomic<unsigned> leakedReports{0};
    std::string audienceError;
    const std::string sentinel = "PRIVATE_TASK_OBSERVER_SENTINEL";
    ReviaSession session(CompanionPaths(temporary.root, {"task-observer", "Task observer fixture", "assistant", false}));
    ReviaSessionTestAccess::MarkStudioStarted(session, true);
    const auto subscription = session.Events().Subscribe(
        [&](const RuntimeEvent& event)
        {
            if (event.kind == RuntimeEventKind::ComponentStatus && event.component == "Task" &&
                event.phase == revia::goals::ToString(revia::goals::GoalStatus::Succeeded))
            {
                audienceChanged.store(
                    session.SetAudience({revia::identity::AudienceKind::Public, "public-observer", 0, {"visitor"}}, audienceError));
                return;
            }
            if (audienceChanged.load() &&
                (event.kind == RuntimeEventKind::StateChanged || event.kind == RuntimeEventKind::AssistantMessage) &&
                (event.message.find(sentinel) != std::string::npos || event.detail.find(sentinel) != std::string::npos))
            {
                leakedReports.fetch_add(1);
            }
        });
    std::string error;
    Check(ReviaSessionTestAccess::LaunchTask(
              session, sentinel,
              [&](std::stop_token stop)
              {
                  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                  while (!release.load() && !stop.stop_requested() && std::chrono::steady_clock::now() < until)
                      std::this_thread::sleep_for(std::chrono::milliseconds(2));
                  revia::goals::Goal goal;
                  goal.title = sentinel;
                  goal.status = revia::goals::GoalStatus::Succeeded;
                  return goal;
              },
              error),
        error);
    release.store(true);
    ReviaSessionTestAccess::WaitForTask(session);
    session.Events().Unsubscribe(subscription);
    ReviaSessionTestAccess::MarkStudioStarted(session, false);
    Check(audienceChanged.load() && session.Audience().kind == revia::identity::AudienceKind::Public,
        "The actual task completion observer did not switch the audience: " + audienceError);
    Check(!session.HasRunningTask() && ReviaSessionTestAccess::FinishedTask(session).find(sentinel) != std::string::npos,
        "The task did not settle or retain its private completion receipt.");
    std::cout << "Task completion observer changed the audience; later private events: " << leakedReports.load() << ".\n";
    Check(leakedReports.load() == 0, "A task observer changed the audience but later private state or report content was published.");
}

void TestReviewedMemoryReceipt()
{
    ScopedTestDirectory temporary;
    const auto database = (temporary.root / "memory.db").string();
    revia::agents::MemoryAgent memory(database);
    memory.SetQueueLimits({64, 0, 0, 512});
    messageRouter router(database);
    memoryDecision decision;
    decision.bSuccess = decision.bShouldRemember = true;
    decision.category = "planning";
    decision.source = "reviewed_lesson:fixture";
    decision.summary = "Verify the result before claiming the task is complete.";
    std::string receipt;
    const auto disposition = memory.SubmitLearnedFinding(router, decision, 0, &receipt);
    Check(disposition != revia::agents::LearnedFindingResult::Failed && !receipt.empty(),
        "Accepted reviewed content did not return its durable memory row receipt.");
    const auto rows = longTermMemory(database).Load();
    Check(rows.size() == 1 && rows.front().id == receipt && rows.front().source == decision.source,
        "Review receipt does not identify the one saved row with its source provenance.");
    std::string duplicateReceipt;
    Check(memory.SubmitLearnedFinding(router, decision, 0, &duplicateReceipt) != revia::agents::LearnedFindingResult::Failed &&
        duplicateReceipt == receipt && longTermMemory(database).Load().size() == 1,
        "Retry after private journal failure duplicated accepted semantic content.");
    memory.SetAdmissionGuard([] { return false; });
    receipt = "old-receipt";
    Check(memory.SubmitLearnedFinding(router, decision, 0, &receipt) == revia::agents::LearnedFindingResult::Failed && receipt.empty(),
        "Denied memory admission returned a stale success receipt.");
}
}

void RunStudioRuntimeTests()
{
    TestReviewedMemoryReceipt();
    TestQueuedMemoryRetainsAdmission();
    TestAudienceContextAndTaskRegistration();
    TestLateTaskKeepsItsLaunchAudience();
    TestTaskObserverCannotChangeReportAudience();
    std::cout << "Connected studio runtime receipt checks passed.\n";
}
