#include "Runtime/conversationRuntime.h"
#include "testSupport.h"
#include "reviaSessionTestAccess.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using namespace revia;
using namespace revia::runtime;
using tests::Check;
using json = nlohmann::json;

class WorkingDirectory
{
  public:
    explicit WorkingDirectory(const std::filesystem::path& root) : previous(std::filesystem::current_path())
    {
        std::filesystem::current_path(root);
    }

    ~WorkingDirectory()
    {
        std::filesystem::current_path(previous);
    }

  private:
    std::filesystem::path previous;
};

class ControlledBackend
{
  public:
    explicit ControlledBackend(std::string modelName = "response-latency-fixture")
    {
        server.Get(
            "/health", [](const auto&, auto& response) { response.set_content(R"({"status":"ok","slots_idle":1})", "application/json"); });
        server.Get("/v1/models", [modelName = std::move(modelName)](const auto&, auto& response)
            { response.set_content(json{{"data", json::array({{{"id", modelName}}})}}.dump(), "application/json"); });
        server.Post("/v1/chat/completions",
            [this](const auto& request, auto& response)
            {
                const json body = json::parse(request.body);
                {
                    std::unique_lock lock(mutex);
                    requestReceived = true;
                    changed.notify_all();
                    changed.wait_for(lock, 5s, [this] { return !holdReply; });
                }
                const std::string answer = "An object file contains compiled code and symbols. The linker resolves its references.";
                if (body.value("stream", false))
                {
                    const json chunk = {{"choices", json::array({{{"delta", {{"content", answer}}}, {"finish_reason", "stop"}}})}};
                    response.set_content("data: " + chunk.dump() + "\n\ndata: [DONE]\n\n", "text/event-stream");
                }
                else
                {
                    response.set_content(
                        json{{"choices", json::array({{{"message", {{"content", answer}}}, {"finish_reason", "stop"}}})}}.dump(),
                        "application/json");
                }
            });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "The response latency fixture could not bind its loopback backend.");
        listener = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        if (!server.is_running())
        {
            server.stop();
            listener.join();
            Check(false, "The response latency fixture backend did not start.");
        }
    }

    ~ControlledBackend()
    {
        Release();
        server.stop();
        if (listener.joinable())
            listener.join();
    }

    void Hold()
    {
        std::lock_guard lock(mutex);
        holdReply = true;
        requestReceived = false;
    }

    bool WaitForRequest()
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [this] { return requestReceived; });
    }

    bool ReceivedRequest()
    {
        std::lock_guard lock(mutex);
        return requestReceived;
    }

    void Release()
    {
        {
            std::lock_guard lock(mutex);
            holdReply = false;
        }
        changed.notify_all();
    }

    int port = 0;

  private:
    httplib::Server server;
    std::mutex mutex;
    std::condition_variable changed;
    bool requestReceived = false;
    bool holdReply = false;
    std::jthread listener;
};

class Fixture
{
  public:
    Fixture()
        : cwd(directory.root), router((directory.root / "router.db").string()), coordinator((directory.root / "agent.db").string()),
          log(directory.root / "Logs"), runtime(
                                            router, context, coordinator, speech, affect, emotions, events, log,
                                            [](RuntimeState, const std::string&) {}, [](const AffectSnapshot&) {}, {}, {}, {},
                                            []
                                            {
                                                responseFilterSettings filters;
                                                filters.bAiReviewEnabled = false;
                                                return filters;
                                            },
                                            {}, {}, {}, {}, {}, {},
                                            []
                                            {
                                                agents::SelfInquiryLimits limits;
                                                limits.enabled = false;
                                                return limits;
                                            })
    {
        profile.id = "response-latency-fixture";
        profile.displayName = "Fixture Revia";
        profile.systemPrompt = "You are Revia. Answer the current supplied question.";
        profile.bMemoryEnabled = false;
        profile.answerObligation = AnswerObligationMode::Reliable;
        llmSettings settings;
        settings.host = "127.0.0.1";
        settings.port = backend.port;
        settings.modelName = profile.id;
        settings.bAutoStartServer = settings.bShutdownServerOnExit = false;
        settings.bVisionEnabled = settings.bAutoMaxTokens = false;
        settings.maxTokens = 256;
        embeddingSettings embeddings;
        embeddings.bEnabled = embeddings.bAutoStartServer = false;
        router.ApplyLLMSettings(settings, embeddings, profile);
        events.BindOrigin(origin, [this](const RuntimeStamp& stamp) { return stamp.SameSession(origin); });
        runtime.ResetResponseLatency(origin);
        subscription = events.Subscribe(
            [this](const RuntimeEvent& event)
            {
                if (event.component == "Response timing")
                    ++timingSummaries;
                if (event.component == "Response latency" && event.phase == "Cancelled")
                    cancelledTurn.store(event.turnId);
            });
    }

    ~Fixture()
    {
        events.Unsubscribe(subscription);
    }

    SessionResult Reply(const std::stop_token stopToken, const std::chrono::steady_clock::time_point acceptedAt)
    {
        identity::AudienceContext audience;
        audience.kind = identity::AudienceKind::Private;
        return runtime.ReplyForAudience(
            "Explain what an object file contains and how the linker uses it.", {}, audience, {}, profile, true, false, stopToken,
            [] { return true; }, {}, acceptedAt);
    }

    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd;
    ControlledBackend backend;
    messageRouter router;
    conversationContext context;
    agents::TurnCoordinator coordinator;
    speech::SpeechService speech;
    AffectController affect;
    emotion::EmotionRuntime emotions;
    RuntimeEventBus events;
    logger log;
    ConversationRuntime runtime;
    aiProfile profile;
    RuntimeStamp origin{"response-latency-companion", "response-latency-session", 1, {}, {}, 0};
    RuntimeEventBus::SubscriptionId subscription = 0;
    std::atomic<std::size_t> timingSummaries{0};
    std::atomic<std::uint64_t> cancelledTurn{0};
};

void TestAcceptedInputOffsetReachesActualReply()
{
    Fixture fixture;
    const auto acceptedAt = std::chrono::steady_clock::now() - 3s;
    const auto reply = fixture.Reply({}, acceptedAt);
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - acceptedAt).count();
    const auto measured = fixture.runtime.ResponseLatencies();
    Check(reply.succeeded && !reply.text.empty(), "The actual controlled latency reply failed: " + reply.reason);
    Check(measured.origin.SameSession(fixture.origin) && measured.turnId != 0 && measured.text.samples == 1,
        "The actual admitted reply lost its origin, correlation, or text sample.");
    Check(measured.textMilliseconds >= 2990.0 && measured.textMilliseconds <= elapsed + 50.0,
        "Actual ReplyForAudience timing omitted the three seconds before generation entry.");
    Check(measured.audioReady.samples == 0 && measured.audioPlayed.samples == 0 && fixture.cancelledTurn.load() == 0,
        "A successful text-only turn invented audio or was retired as cancelled.");
}

void TestSharedSessionRetainsAcceptedInputOffset()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    ControlledBackend backend("fixture-main");
    ReviaSession session(CompanionPaths(directory.root, {"shared-latency-fixture", "Fixture Revia", "assistant", false}));
    ReviaSessionTestAccess::ConfigureStartupBrains(session, backend.port);
    ReviaSessionTestAccess::MarkStudioStarted(session, true);
    std::string error;
    Check(session.SetAudience({identity::AudienceKind::Shared, "shared-latency", 0, {"fixture-viewer"}}, error), error);
    agents::InputBatch batch;
    batch.text = "Explain what an object file contains and how the linker uses it.";
    batch.context = ReviaSessionTestAccess::StudioInput(session, agents::InputSource::Typed);
    batch.acceptedAt = std::chrono::steady_clock::now() - 3s;
    const auto reply = ReviaSessionTestAccess::RunAcceptedTurn(session, batch);
    const auto measured = ReviaSessionTestAccess::ResponseTiming(session);
    ReviaSessionTestAccess::MarkStudioStarted(session, false);
    Check(reply.succeeded && !reply.text.empty(), "The actual shared Session timing reply failed: " + reply.reason);
    Check(measured.text.samples == 1 && measured.textMilliseconds >= 2990.0,
        "The shared Session lost accepted input time before entering conversation generation.");
}

void TestHeldReplyCancellationRetiresTimingAndSpeechCorrelation()
{
    Fixture fixture;
    fixture.backend.Hold();
    std::stop_source cancellation;
    SessionResult reply;
    std::exception_ptr failure;
    std::jthread worker(
        [&]
        {
            try
            {
                reply = fixture.Reply(cancellation.get_token(), std::chrono::steady_clock::now());
            }
            catch (...)
            {
                failure = std::current_exception();
            }
        });
    const bool requested = fixture.backend.WaitForRequest();
    const auto pending = fixture.runtime.ResponseLatencies();
    const auto summariesBefore = fixture.timingSummaries.load();
    RuntimeEvent binding;
    binding.stamp = fixture.origin;
    binding.kind = RuntimeEventKind::Timing;
    binding.component = "Response latency";
    binding.phase = "SpeechQueued";
    binding.turnId = pending.turnId;
    binding.utteranceId = 9001;
    // No audio worker runs. Bind fixture speech to the actual pending runtime turn.
    fixture.events.Publish(binding);
    const bool bound = fixture.timingSummaries.load() > summariesBefore;
    std::atomic<std::size_t> lateEvents{0};
    const auto cancellationSubscription = fixture.events.Subscribe(
        [&](const RuntimeEvent&)
        {
            if (cancellation.stop_requested())
                ++lateEvents;
        });
    const auto summariesBeforeCancellation = fixture.timingSummaries.load();
    cancellation.request_stop();
    fixture.backend.Release();
    worker.join();
    fixture.events.Unsubscribe(cancellationSubscription);
    if (failure)
        std::rethrow_exception(failure);
    Check(requested && pending.turnId != 0 && pending.textMilliseconds < 0.0 && bound,
        "The cancellation fixture never held an actual pending reply with a registered speech correlation.");
    Check(!reply.succeeded && reply.text.empty(), "The held reply escaped cancellation into delivered text.");
    const auto retired = fixture.runtime.ResponseLatencies();
    Check(retired.origin.SameSession(pending.origin) && retired.turnId == 0 && retired.text.samples == 0 &&
              retired.audioReady.samples == 0 && retired.audioPlayed.samples == 0,
        "Actual runtime cancellation left the pending response timing active.");
    Check(lateEvents.load() == 0 && fixture.cancelledTurn.load() == 0 && fixture.timingSummaries.load() == summariesBeforeCancellation,
        "Cancelled runtime published a late event or response timing summary after admission retired.");
    const auto summariesAfterRetirement = fixture.timingSummaries.load();
    for (const std::string phase : {"FirstAudioReady", "FirstAudioPlayed"})
    {
        RuntimeEvent late;
        late.stamp = fixture.origin;
        late.component = "Voice";
        late.phase = phase;
        late.turnId = binding.utteranceId;
        late.utteranceId = binding.utteranceId;
        fixture.events.Publish(std::move(late));
    }
    const auto afterLateAudio = fixture.runtime.ResponseLatencies();
    Check(afterLateAudio.audioReady.samples == 0 && afterLateAudio.audioPlayed.samples == 0 &&
              fixture.timingSummaries.load() == summariesAfterRetirement,
        "Late audio from the cancelled utterance reentered actual runtime timing.");
}

void TestProactiveRepliesRetainProducerAdmission()
{
    Fixture fixture;
    std::size_t recaptures = 0;
    fixture.runtime.SetPrivateAdmissionFactory(
        [&]
        {
            ++recaptures;
            return [] { return true; };
        });
    const auto retiredAdmission = [] { return false; };
    const auto opening = fixture.runtime.StartConversation(
        "old private cue", "old private evidence", fixture.profile, true, false, {}, 41, retiredAdmission);
    const auto thought = fixture.runtime.StartCuriosityConversation(
        "old private topic", "old rationale", "old references", fixture.profile, true, false, {}, 41, retiredAdmission);
    Check(!opening.succeeded && opening.text.empty() && !thought.succeeded && thought.text.empty() && recaptures == 0,
        "An autonomous reply adopted fresh private admission after its producer's audience had retired.");
    Check(fixture.context.GetRecentMessages().empty() && !fixture.backend.ReceivedRequest(),
        "A retired autonomous producer entered history or called the model before refusal.");
}
}

void RunResponseLatencyRuntimeTests()
{
    TestAcceptedInputOffsetReachesActualReply();
    TestSharedSessionRetainsAcceptedInputOffset();
    TestHeldReplyCancellationRetiresTimingAndSpeechCorrelation();
    TestProactiveRepliesRetainProducerAdmission();
    std::cout << "Actual response latency offset, cancellation, and late audio checks passed.\n";
}
