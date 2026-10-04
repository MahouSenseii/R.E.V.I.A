#include "Agents/memoryAgent.h"
#include "Actions/actionRuntime.h"
#include "Memory/longTermMemory.h"
#include "Runtime/runtimeEvents.h"
#include "testSupport.h"
#include "reviaSessionTestAccess.h"

#include <fstream>
#include <iostream>
#include <atomic>
#include <chrono>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifdef CreateDirectory
#undef CreateDirectory
#endif

namespace
{
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;

struct NativeActionFixture
{
    ScopedTestDirectory temporary;
    revia::runtime::ReviaSession session;
    std::filesystem::path approved = temporary.root / "approved";
    std::filesystem::path marker = approved / "marker.txt";

    explicit NativeActionFixture(const char* mode = "approved_scope")
        : session(revia::runtime::CompanionPaths(temporary.root, {"native-action", "Native action fixture", "assistant", false}))
    {
        std::filesystem::create_directories(approved);
        {
            std::ofstream output(marker);
            output << "PRIVATE_NATIVE_ACTION_SENTINEL_5F9A";
        }
        {
            std::ofstream output(temporary.root / "capabilities.json");
            output << nlohmann::json{{"mode", mode}, {"approvedRoots", {revia::actions::PathToUtf8(approved)}},
                {"autoApproveRiskThrough", "read_only"}, {"createMissingApprovedRoots", false}}
                          .dump();
        }
        std::string error;
        Check(revia::runtime::ReviaSessionTestAccess::Actions(session).Initialize(
                  temporary.root / "capabilities.json", temporary.root / "action-audit.jsonl", error),
            error);
        revia::runtime::ReviaSessionTestAccess::MarkStudioStarted(session, true);
    }

    ~NativeActionFixture()
    {
        revia::runtime::ReviaSessionTestAccess::Actions(session).SetDispatchObserver({});
        revia::runtime::ReviaSessionTestAccess::MarkStudioStarted(session, false);
    }

    revia::actions::ActionRequest Request(const revia::actions::ActionType type) const
    {
        revia::actions::ActionRequest request;
        request.id = revia::actions::NewActionId();
        request.type = type;
        request.source = type == revia::actions::ActionType::ReadTextFile ? marker : approved;
        return request;
    }
};

struct ScopedRuntimeSubscription
{
    revia::runtime::RuntimeEventBus& events;
    revia::runtime::RuntimeEventBus::SubscriptionId id;
    ~ScopedRuntimeSubscription()
    {
        events.Unsubscribe(id);
    }
};

void TestNativeActionEventsRetainCapturedAudience()
{
    using namespace revia::runtime;
    NativeActionFixture fixture;
    const auto origin = fixture.session.Stamp();
    const auto audience = fixture.session.Audience();
    std::vector<RuntimeEvent> observed;
    ScopedRuntimeSubscription subscription{
        fixture.session.Events(), fixture.session.Events().Subscribe(
                                      [&](const RuntimeEvent& event)
                                      {
                                          if (event.component == "Automation" || event.kind == RuntimeEventKind::StateChanged)
                                              observed.push_back(event);
                                      })};
    const auto read = ReviaSessionTestAccess::RunStudioAction(fixture.session, fixture.Request(revia::actions::ActionType::ReadTextFile));
    const auto listed =
        ReviaSessionTestAccess::RunStudioAction(fixture.session, fixture.Request(revia::actions::ActionType::ListDirectory));
    Check(read.succeeded && read.text.find("PRIVATE_NATIVE_ACTION_SENTINEL_5F9A") != std::string::npos && listed.succeeded &&
              listed.text.find("marker.txt") != std::string::npos,
        "The approved native read/list fixture did not return its actual file evidence.");
    Check(read.audienceRevision == audience.revision && listed.audienceRevision == audience.revision && read.stamp.SameSession(origin) &&
              listed.stamp.SameSession(origin),
        "Native command results lost their captured audience or session stamp.");
    unsigned running = 0, completed = 0, states = 0;
    for (const auto& event : observed)
    {
        Check(event.stamp.SameSession(origin) && event.stamp.policyVersion == origin.policyVersion &&
                  event.audienceRevision == audience.revision,
            "A native Automation or State event lacked its captured audience revision and policy stamp.");
        if (event.component == "Automation")
        {
            running += event.phase == "Running";
            completed += event.phase == "Ready";
        }
        states += event.kind == RuntimeEventKind::StateChanged;
    }
    Check(running == 2 && completed == 2 && states >= 2, "Native action metadata checks never observed actual producer events.");
}

void TestNativeActionStartedSubscriberCannotRecaptureAudience()
{
    using namespace revia::runtime;
    NativeActionFixture fixture;
    const auto origin = fixture.session.Stamp();
    const auto audience = fixture.session.Audience();
    bool flipped = false;
    unsigned running = 0, lateCompletions = 0, lateStates = 0, dispatched = 0;
    std::string error;
    ReviaSessionTestAccess::Actions(fixture.session)
        .SetDispatchObserver([&](const auto&, const bool beginning) { dispatched += beginning; });
    ScopedRuntimeSubscription subscription{fixture.session.Events(),
        fixture.session.Events().Subscribe(
            [&](const RuntimeEvent& event)
            {
                if (event.component == "Automation" && event.phase == "Running")
                {
                    ++running;
                    Check(event.stamp.SameSession(origin) && event.audienceRevision == audience.revision,
                        "The native Running event was stamped after its admission boundary.");
                    flipped =
                        fixture.session.SetAudience({revia::identity::AudienceKind::Public, "action-observer", 0, {"visitor"}}, error);
                    return;
                }
                if (flipped)
                {
                    lateCompletions += event.component == "Automation";
                    lateStates += event.kind == RuntimeEventKind::StateChanged;
                }
            })};
    const auto result = ReviaSessionTestAccess::RunStudioAction(fixture.session, fixture.Request(revia::actions::ActionType::ReadTextFile));
    Check(flipped && running == 1 && fixture.session.Audience().revision != audience.revision,
        "The native producer boundary did not change audience.");
    Check(!result.succeeded && result.text.empty() && !result.reason.empty() && dispatched == 0,
        "A native read executed or disclosed its result after a Running subscriber revoked its captured audience.");
    Check(lateCompletions == 0 && lateStates == 0, "A revoked native action published a completion or state into a new audience.");
}

void TestNativeActionConfirmationCannotRecaptureAudience(const bool cancel)
{
    using namespace revia::runtime;
    NativeActionFixture fixture("supervised");
    const auto destination = fixture.approved / "not-created-after-revocation";
    revia::actions::ActionRequest request;
    request.id = revia::actions::NewActionId();
    request.type = revia::actions::ActionType::CreateDirectory;
    request.source = destination;
    const auto policy = ReviaSessionTestAccess::Actions(fixture.session).Evaluate(request);
    Check(policy.verdict == revia::actions::PolicyVerdict::RequiresConfirmation,
        "The native confirmation fixture did not enter the existing policy approval path: " + policy.reason);
    bool flipped = false;
    unsigned running = 0;
    std::string error;
    fixture.session.SetConfirmationHandler(
        [&](const auto&, const auto&)
        {
            flipped = fixture.session.SetAudience({revia::identity::AudienceKind::Public, "confirmation-observer", 0, {"visitor"}}, error);
            if (cancel)
                fixture.session.RequestStop();
            return revia::actions::ConfirmationChoice::Allow;
        });
    ScopedRuntimeSubscription subscription{fixture.session.Events(),
        fixture.session.Events().Subscribe(
            [&](const RuntimeEvent& event) { running += event.component == "Automation" && event.phase == "Running"; })};
    const auto result = ReviaSessionTestAccess::RunStudioAction(fixture.session, std::move(request));
    Check(flipped && !result.succeeded && result.text.empty() && running == 0 && !std::filesystem::exists(destination),
        "Confirmation approval recaptured a new audience and executed a revoked native action.");
}

void TestNativeActionCancellationNoticeCannotChangeAudience()
{
    using namespace revia::runtime;
    NativeActionFixture fixture("supervised");
    auto request = fixture.Request(revia::actions::ActionType::CreateDirectory);
    request.source = fixture.approved / "not-created-after-stop";
    const auto destination = request.source;
    bool prompted = false, flipped = false;
    unsigned running = 0;
    std::string error;
    fixture.session.SetConfirmationHandler(
        [&](const auto&, const auto&)
        {
            prompted = true;
            fixture.session.RequestStop();
            return revia::actions::ConfirmationChoice::Allow;
        });
    ScopedRuntimeSubscription subscription{fixture.session.Events(),
        fixture.session.Events().Subscribe(
            [&](const RuntimeEvent& event)
            {
                running += event.component == "Automation" && event.phase == "Running";
                if (event.kind == RuntimeEventKind::StateChanged && event.state == RuntimeState::Idle)
                    flipped =
                        fixture.session.SetAudience({revia::identity::AudienceKind::Public, "cancel-observer", 0, {"visitor"}}, error);
            })};
    const auto result = ReviaSessionTestAccess::RunStudioAction(fixture.session, std::move(request));
    Check(prompted && flipped && !result.succeeded && result.text.empty() && running == 0 && !std::filesystem::exists(destination),
        "A cancellation notice delivered action text after its state subscriber changed the audience.");
}

void TestNativeActionWaitingSubscriberCannotPrompt()
{
    using namespace revia::runtime;
    for (const bool cancel : {false, true})
    {
        NativeActionFixture fixture("supervised");
        auto request = fixture.Request(revia::actions::ActionType::CreateDirectory);
        request.source = fixture.approved / "not-created-from-retired-prompt";
        const auto destination = request.source;
        bool waiting = false, prompted = false;
        std::string error;
        fixture.session.SetConfirmationHandler(
            [&](const auto&, const auto&)
            {
                prompted = true;
                return revia::actions::ConfirmationChoice::Allow;
            });
        ScopedRuntimeSubscription subscription{fixture.session.Events(),
            fixture.session.Events().Subscribe(
                [&](const RuntimeEvent& event)
                {
                    if (event.kind != RuntimeEventKind::StateChanged || event.state != RuntimeState::WaitingForConfirmation)
                        return;
                    waiting = true;
                    if (cancel)
                        fixture.session.RequestStop();
                    else
                        Check(
                            fixture.session.SetAudience({revia::identity::AudienceKind::Public, "waiting-observer", 0, {"visitor"}}, error),
                            error);
                })};
        const auto result = ReviaSessionTestAccess::RunStudioAction(fixture.session, std::move(request));
        Check(waiting && !prompted && !result.succeeded && !std::filesystem::exists(destination),
            "A waiting-state observer retired admission, but the session still invoked a private approval prompt.");
        Check(cancel ? result.text.find("cancelled before execution") != std::string::npos : result.text.empty(),
            "Pre-prompt retirement lost same-context cancellation or disclosed text into a new audience.");
    }
}

void TestNativeActionCompletionSubscriberCannotDeliverRevokedResult()
{
    using namespace revia::runtime;
    for (const bool revokeAtState : {false, true})
    {
        NativeActionFixture fixture;
        bool flipped = false;
        unsigned dispatched = 0, lateStates = 0;
        std::string error;
        ReviaSessionTestAccess::Actions(fixture.session)
            .SetDispatchObserver([&](const auto&, const bool beginning) { dispatched += beginning; });
        ScopedRuntimeSubscription subscription{fixture.session.Events(),
            fixture.session.Events().Subscribe(
                [&](const RuntimeEvent& event)
                {
                    const bool target = revokeAtState ? event.kind == RuntimeEventKind::StateChanged && event.state == RuntimeState::Idle
                                                      : event.component == "Automation" && event.phase == "Ready";
                    if (!flipped && target)
                    {
                        flipped = fixture.session.SetAudience(
                            {revia::identity::AudienceKind::Public, "completion-observer", 0, {"visitor"}}, error);
                        return;
                    }
                    if (flipped && event.kind == RuntimeEventKind::StateChanged)
                        ++lateStates;
                })};
        const auto result =
            ReviaSessionTestAccess::RunStudioAction(fixture.session, fixture.Request(revia::actions::ActionType::ReadTextFile));
        Check(flipped && dispatched == 1, "The post-execution native callback did not revoke an actual successful read.");
        Check(!result.succeeded && result.text.empty() && !result.reason.empty() && lateStates == 0,
            "A native completion or final-state subscriber revoked admission, but private file text or later state was still delivered.");
    }
}

class HeldMemoryBackend
{
  public:
    explicit HeldMemoryBackend(std::string supplied = {}) : suppliedResponse(std::move(supplied))
    {
        server.Get("/health", [](const auto&, auto& response) { response.set_content(R"({"status":"ok"})", "application/json"); });
        server.Get("/v1/models",
            [](const auto&, auto& response) { response.set_content(R"({"data":[{"id":"held-memory"}]})", "application/json"); });
        server.Post("/v1/chat/completions",
            [this](const auto& request, auto& response)
            {
                {
                    std::lock_guard lock(requestMutex);
                    latestRequest = nlohmann::json::parse(request.body);
                }
                ++requests;
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!release.load() && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                const auto answer = nlohmann::json{{"choices",
                    nlohmann::json::array(
                        {{{"message", {{"content", suppliedResponse.empty() ? R"({"shouldRemember":false,"reason":"Synthetic fixture."})"
                                                                            : suppliedResponse}}},
                            {"finish_reason", "stop"}}})}};
                response.set_content(answer.dump(), "application/json");
            });
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Held memory backend could not bind.");
        worker = std::jthread([this] { server.listen_after_bind(); });
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!server.is_running() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Check(server.is_running(), "Held memory backend could not start.");
    }
    ~HeldMemoryBackend()
    {
        release.store(true);
        server.stop();
        if (worker.joinable())
            worker.join();
    }
    int port = 0;
    std::atomic<unsigned> requests{0};
    std::atomic<bool> release{false};

    nlohmann::json LatestRequest()
    {
        std::lock_guard lock(requestMutex);
        return latestRequest;
    }

  private:
    std::mutex requestMutex;
    nlohmann::json latestRequest;
    httplib::Server server;
    std::jthread worker;
    std::string suppliedResponse;
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
    while (backend.requests.load() == 0 && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Check(backend.requests.load() == 1, "The first real memory classification did not enter the held transport.");
    std::atomic<bool> current{true};
    std::atomic<bool> denied{false};
    memory.Submit(router, "PRIVATE_QUEUED_SENTINEL_9f32", {}, revia::agents::ResponseProvenance::NormalGeneration, 2,
        [&]
        {
            if (!current.load())
                denied.store(true);
            return current.load();
        });
    Check(memory.Depths().interactive == 1, "The context-bound second classification was not actually queued.");
    current.store(false);
    backend.release.store(true);
    while (!denied.load() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
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
    revia::identity::SpeakerObservation observed{
        "person-b", revia::identity::SpeakerSource::ConsentedVoice, "synthetic-observation", *revision, 0.9F};
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
    const auto subscription = session.Events().Subscribe(
        [&](const RuntimeEvent& event)
        {
            if (event.kind == RuntimeEventKind::AssistantMessage && event.component == "Task")
                delivered.fetch_add(1);
        });
    std::string error;
    Check(ReviaSessionTestAccess::LaunchTask(
              session, "PRIVATE_LATE_TASK_SENTINEL",
              [&](std::stop_token stop)
              {
                  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                  while (!release.load() && !stop.stop_requested() && std::chrono::steady_clock::now() < until)
                      std::this_thread::sleep_for(std::chrono::milliseconds(2));
                  revia::goals::Goal goal;
                  goal.title = "PRIVATE_LATE_TASK_SENTINEL";
                  goal.status = revia::goals::GoalStatus::Succeeded;
                  return goal;
              },
              error),
        error);
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

void TestLocalStudioIncompleteVerificationCannotAdvance()
{
    using namespace revia::runtime;
    ScopedTestDirectory temporary;
    HeldMemoryBackend backend(
        R"({"summary":"A complete three-step plan is provided.","evidence":"The supplied objective supports the plan.","verified":true})");
    backend.release.store(true);
    ReviaSession session(CompanionPaths(temporary.root, {"studio-contract", "Studio contract fixture", "assistant", false}));
    ReviaSessionTestAccess::ConfigureStartupBrains(session, backend.port);
    ReviaSessionTestAccess::MarkStudioStarted(session, true);
    std::string error;
    Check(session.StartAgentWorkflow("Propose read-only steps for the explicitly supplied marker file.", false, error), error);
    ReviaSessionTestAccess::JoinWorkflow(session);
    const auto snapshot = session.AgentWorkflowSnapshot();
    unsigned failedWorkers = 0;
    for (const auto& node : snapshot.nodes)
    {
        if (node.role == revia::agents::WorkflowRole::Worker && node.state == revia::agents::WorkflowState::Failed)
            ++failedWorkers;
        if (node.role == revia::agents::WorkflowRole::Reviewer || node.role == revia::agents::WorkflowRole::Parent)
            Check(node.attempts.empty(), "An incomplete local worker artifact reached review or parent acceptance.");
    }
    Check(backend.requests.load() == 2 && failedWorkers == 2 && session.AgentWorkflowResult().empty(),
        "Local provider verified:true bypassed task-owned deliverable completeness.");
    ReviaSessionTestAccess::MarkStudioStarted(session, false);
}

void TestLocalStudioGrammarPinsNativeEvidence()
{
    using namespace revia::runtime;
    using namespace revia::agents;
    using json = nlohmann::json;
    const json references = json::array({{{"nodeId", "analysis"}, {"id", "analysis-artifact"}, {"version", 2}, {"hash", "hash-a"}},
        {{"nodeId", "verification"}, {"id", "verification-artifact"}, {"version", 1}, {"hash", "hash-b"}}});
    for (int variant = 0; variant < 3; ++variant)
    {
        ScopedTestDirectory temporary;
        const json payload = {{"summary", "A bounded plan."}, {"evidence", "Supplied analytical material only."},
            {"verified", variant != 2}, {"prerequisiteEvidence", variant == 0 ? json::array() : references},
            {"steps", {{"items", {"Inspect the supplied marker evidence."}}, {"noneReason", ""}}},
            {"risks", {{"items", json::array()}, {"noneReason", "This task only analyzes supplied text without effects."}}}};
        HeldMemoryBackend backend(payload.dump());
        backend.release.store(true);
        ReviaSession session(CompanionPaths(temporary.root, {"schema-fixture", "Schema fixture", "assistant", false}));
        ReviaSessionTestAccess::ConfigureStartupBrains(session, backend.port);
        NodeRequest request;
        request.stamp = session.Stamp();
        request.node.id = "review";
        request.node.role = WorkflowRole::Reviewer;
        request.node.objective = "Assess supplied evidence without executing anything.";
        request.node.deliverableContract = {{{DeliverableSection::Steps, "Give concrete steps.", false},
                                                {DeliverableSection::Risks, "Identify actual risks or explain their absence.", true}},
            true};
        request.prerequisiteReferences = {
            {"analysis", "analysis-artifact", 2, "hash-a"}, {"verification", "verification-artifact", 1, "hash-b"}};
        const auto result = ReviaSessionTestAccess::RunAgentProvider(session, request);
        Check(result.succeeded && result.verified == (variant == 1),
            "Native provider validation accepted missing evidence or a false verification claim, or rejected a valid control.");
        const auto wire = backend.LatestRequest();
        Check(wire.at("response_format").at("type") == "json_schema", "Studio did not transmit a constrained response schema.");
        const auto& properties = wire.at("response_format").at("json_schema").at("schema").at("properties");
        Check(properties.at("prerequisiteEvidence").contains("const") && properties.at("prerequisiteEvidence").at("const") == references,
            "Model grammar permits omitted or fabricated prerequisite identity/version/hash.");
        const auto& steps = properties.at("steps").at("properties");
        Check(steps.at("items").value("minItems", 0) == 1 && steps.at("noneReason").at("const") == "",
            "Model grammar permits empty required steps or a contradictory absence reason.");
        const auto& alternatives = properties.at("risks").at("anyOf");
        Check(alternatives.size() == 2 && alternatives.at(0).at("properties").at("items").at("minItems") == 1 &&
                  alternatives.at(1).at("properties").at("items").at("maxItems") == 0 &&
                  alternatives.at(1).at("properties").at("noneReason").at("minLength") == 1,
            "Model grammar lost mutually exclusive optional-section content and absence rationale.");
    }
}

void TestStudioDiagnosticRetainsChangedEvidenceRecovery()
{
    using namespace revia::runtime;
    ScopedTestDirectory temporary;
    ReviaSession session(CompanionPaths(temporary.root, {"studio-diagnostic", "Studio diagnostic fixture", "assistant", false}));
    std::string error;
    Check(session.StartAgentWorkflow("Bounded synthetic diagnostic objective.", true, error), error);
    ReviaSessionTestAccess::JoinWorkflow(session);
    Check(!session.DecideAgentWorkflow(revia::agents::ParentDecision::Accept, error), "Incomplete diagnostic evidence was accepted.");
    Check(session.RetryAgentNode("verification", "Changed diagnostic input", "changed-diagnostic-evidence", error), error);
    ReviaSessionTestAccess::JoinWorkflow(session);
    Check(session.DecideAgentWorkflow(revia::agents::ParentDecision::Accept, error), "Separate parent diagnostic evaluation was refused.");
    ReviaSessionTestAccess::JoinWorkflow(session);
    Check(session.DecideAgentWorkflow(revia::agents::ParentDecision::Accept, error) && !session.AgentWorkflowResult().empty(),
        "Complete diagnostic evidence did not reach separate parent acceptance.");
}
}

void RunStudioRuntimeTests()
{
    TestNativeActionEventsRetainCapturedAudience();
    TestNativeActionStartedSubscriberCannotRecaptureAudience();
    TestNativeActionConfirmationCannotRecaptureAudience(false);
    TestNativeActionConfirmationCannotRecaptureAudience(true);
    TestNativeActionCancellationNoticeCannotChangeAudience();
    TestNativeActionWaitingSubscriberCannotPrompt();
    TestNativeActionCompletionSubscriberCannotDeliverRevokedResult();
    TestReviewedMemoryReceipt();
    TestLocalStudioIncompleteVerificationCannotAdvance();
    TestLocalStudioGrammarPinsNativeEvidence();
    TestStudioDiagnosticRetainsChangedEvidenceRecovery();
    TestQueuedMemoryRetainsAdmission();
    TestAudienceContextAndTaskRegistration();
    TestLateTaskKeepsItsLaunchAudience();
    TestTaskObserverCannotChangeReportAudience();
    std::cout << "Connected studio runtime receipt checks passed.\n";
}
