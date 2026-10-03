#include "reviaSessionTestAccess.h"
#include "speechServiceTestAccess.h"
#include "promptLayoutTestSupport.h"
#include "Diagnostics/issueLog.h"
#include "Runtime/conversationRuntime.h"
#include "Speech/systemCueBank.h"
#include "Speech/vocalization.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

namespace
{
using namespace revia;
using namespace revia::runtime;
using tests::Check;
using json = nlohmann::json;

class FixtureEnvironment
{
  public:
    explicit FixtureEnvironment(const std::filesystem::path& root) : previousDirectory(std::filesystem::current_path())
    {
        if (const char* value = std::getenv("REVIA_LOG_DIR"))
            previousLogDirectory = value;
        std::filesystem::current_path(root);
        SetLogDirectory((root / "Logs").string());
    }

    ~FixtureEnvironment()
    {
        std::filesystem::current_path(previousDirectory);
        SetLogDirectory(previousLogDirectory);
    }

  private:
    static void SetLogDirectory(const std::string& value)
    {
#ifdef _WIN32
        _putenv_s("REVIA_LOG_DIR", value.c_str());
#else
        if (value.empty())
            unsetenv("REVIA_LOG_DIR");
        else
            setenv("REVIA_LOG_DIR", value.c_str(), 1);
#endif
    }

    std::filesystem::path previousDirectory;
    std::string previousLogDirectory;
};

class FaultBackend
{
  public:
    explicit FaultBackend(const std::filesystem::path& root)
    {
        tests::WriteMinimalWav(root / "valid.wav");
        std::ifstream file(root / "valid.wav", std::ios::binary);
        wav.assign(std::istreambuf_iterator<char>(file), {});
        server.Get("/health", [](const auto&, auto& response) { response.set_content("{}", "application/json"); });
        server.Get("/v1/models",
            [](const auto&, auto& response) { response.set_content(R"({"data":[{"id":"fixture-main"}]})", "application/json"); });
        server.Post("/v1/audio/pcm",
            [this](const auto&, auto& response)
            {
                ++synthesisRequests;
                if (fail.load())
                {
                    response.status = 500;
                    response.set_content(
                        R"({"message":"PRIVATE_WORKER_SENTINEL sk-private C:/private/reference.wav"})", "application/json");
                }
                else
                {
                    response.set_header("X-Revia-Audio-Cache-Hit", cached ? "1" : "0");
                    response.set_content(wav, "audio/wav");
                }
            });
        server.Post("/v1/chat/completions",
            [this](const auto& request, auto& response)
            {
                const auto body = json::parse(request.body);
                {
                    std::lock_guard lock(mutex);
                    lastRequest = body;
                }
                if (body.value("stream", false))
                {
                    response.set_content("data: {\"choices\":[{\"delta\":{\"content\":\"The approved reply remains "
                                         "available.\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n",
                        "text/event-stream");
                }
                else
                {
                    response.set_content(
                        R"({"choices":[{"message":{"content":"The approved reply remains available."},"finish_reason":"stop"}]})",
                        "application/json");
                }
            });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind the speech integration fixture.");
        thread = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Check(server.is_running(), "The speech integration fixture did not start.");
    }

    ~FaultBackend()
    {
        server.stop();
        thread.join();
    }
    std::string Posture()
    {
        std::lock_guard lock(mutex);
        return tests::RuntimeAuthoredText(lastRequest);
    }

    int port = 0;
    std::atomic<int> synthesisRequests{0};
    std::atomic<bool> fail{true};
    std::atomic<bool> cached{false};

  private:
    httplib::Server server;
    std::jthread thread;
    std::mutex mutex;
    json lastRequest;
    std::string wav;
};

speechSettings SpeechSettings(const std::filesystem::path& root, int port)
{
    speechSettings settings;
    settings.backend = "Qwen";
    settings.qwenHost = "127.0.0.1";
    settings.qwenPort = port;
    settings.qwenDevices = {"cpu"};
    settings.qwenDevice = "cpu";
    settings.voiceDataPath = (root / "Voices").string();
    return settings;
}

speech::VoicePreset FixtureVoice(const std::filesystem::path& root, std::string id = "voice-A")
{
    speech::VoicePreset voice;
    voice.id = std::move(id);
    voice.referenceAudioPath = (root / "reference.wav").string();
    return voice;
}

std::size_t WarningCount(const std::vector<RuntimeEvent>& events, const std::string& text)
{
    return std::count_if(events.begin(), events.end(),
        [&](const auto& event) { return event.kind == RuntimeEventKind::Warning && event.message.find(text) != std::string::npos; });
}

void TestScopedJournalAndCheckedPersistence()
{
    tests::ScopedTestDirectory directory;
    diagnostics::IssueLog log(directory.root / "issues.jsonl");
    diagnostics::Issue a;
    a.component = "Voice";
    a.code = "synthesis-failed";
    a.profileId = "profile.A";
    a.voiceId = "voice:1";
    a.summary = "Synthesis failed.";
    a.remedy = "Verify fresh synthesis.";
    a.correlationId = "attempt=1";
    auto b = a;
    b.profileId = "profile.B";
    std::string error;
    Check(log.Record(a, error) && log.Record(b, error), "Scoped issues were not recorded.");
    Check(log.OpenCount() == 2, "Different profile faults collided.");
    Check(log.Resolve(a.component, a.code, a.profileId, a.voiceId, &error), "Scoped resolution failed.");
    Check(log.Find(b.component, b.code, b.profileId, b.voiceId)->status == diagnostics::IssueStatus::Open,
        "One profile's recovery cleared another profile's fault.");
    auto delimiter = a;
    delimiter.profileId = "profile.A5:voice";
    delimiter.voiceId = "1";
    Check(delimiter.Key() != a.Key(), "Scoped key encoding was ambiguous.");
    diagnostics::Issue legacy = a;
    legacy.profileId.clear();
    legacy.voiceId.clear();
    Check(legacy.Key() == "Voice/synthesis-failed", "Legacy unscoped identity changed.");
    Check(log.Record(legacy, error) && log.Find(legacy.component, legacy.code), "Legacy issue lookup failed.");
    diagnostics::IssueLog reload(log.Path());
    Check(reload.Load(error) && reload.OpenCount() == 2, "Restart lost scoped or legacy state.");
    Check(reload.Find(a.component, a.code, a.profileId, a.voiceId)->correlationId == "attempt=1",
        "Correlation metadata did not survive persistence.");

    diagnostics::IssueLog unavailable(directory.root);
    Check(!unavailable.Record(a, error) && !error.empty() && unavailable.OpenCount() == 1,
        "A failed journal write erased live health or was reported as saved.");
    Check(!unavailable.Resolve(a.component, a.code, a.profileId, a.voiceId, &error) && !error.empty(),
        "A failed resolution write was reported as saved.");
    Check(unavailable.OpenCount() == 0, "Failed persistence prevented in-memory recovery.");

    {
        std::ofstream journal(log.Path(), std::ios::app);
        journal << std::string(diagnostics::IssueLog::maximumJournalBytes, ' ');
    }
    Check(log.Record(b, error), "Journal compaction failed.");
    Check(std::filesystem::file_size(log.Path()) < diagnostics::IssueLog::maximumJournalBytes,
        "Issue history grew past its compaction threshold.");
    Check(reload.Load(error) && reload.Find(b.component, b.code, b.profileId, b.voiceId)->status == diagnostics::IssueStatus::Open,
        "Journal compaction forgot an unresolved scope.");
}

void TestServiceToSessionHistoryAndRecovery()
{
    tests::ScopedTestDirectory directory;
    FixtureEnvironment environment(directory.root);
    FaultBackend backend(directory.root);
    {
        std::vector<RuntimeEvent> events;
        std::vector<speech::SpeechEvent> healthEvents;
        ReviaSession session;
        ReviaSessionTestAccess::LoadSpeechFaultHistory(session);
        session.Events().Subscribe([&](const auto& event) { events.push_back(event); });
        auto& service = ReviaSessionTestAccess::Speech(session);
        speech::SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(service, SpeechSettings(directory.root, backend.port),
            FixtureVoice(directory.root),
            [&](const auto& event)
            {
                if (event.synthesis)
                    healthEvents.push_back(event);
                ReviaSessionTestAccess::SpeechEvent(session, event);
            });
        ReviaSessionTestAccess::SetSpeakingIntent(session, 10);
        service.Speak("A private approved phrase.", {}, 11);
        speech::SpeechServiceTestAccess::TakeNext(service)();
        auto& issues = ReviaSessionTestAccess::SpeechIssues(session);
        const auto origin = service.SynthesisHealthSnapshot().selection;
        const auto failure = issues.Find("Voice", "synthesis-failed", origin.profileId, origin.presetId);
        Check(failure && failure->status == diagnostics::IssueStatus::Open && failure->occurrences == 1,
            "Real synthesis completion did not reach scoped issue history.");
        Check(ReviaSessionTestAccess::SpeakingIntent(session) == 10, "An early synthesis failure ended another phrase's playback intent.");
        Check(WarningCount(events, "Voice synthesis failed") == 1 && backend.synthesisRequests == 1,
            "Failure reporting recursively synthesized or failed to notify once.");
        const auto terminal =
            *std::find_if(healthEvents.begin(), healthEvents.end(), [](const auto& event) { return event.phase == "SynthesisFailed"; });
        ReviaSessionTestAccess::SpeechEvent(session, terminal);
        service.Speak("Another private phrase.", {}, 12);
        speech::SpeechServiceTestAccess::TakeNext(service)();
        Check(issues.Find("Voice", "synthesis-failed", origin.profileId, origin.presetId)->occurrences == 1 &&
                  WarningCount(events, "Voice synthesis failed") == 1,
            "Duplicate or repeated failures created a notification storm.");
        ReviaSessionTestAccess::SpeechEvent(session, {"Ready", "Ready"});
        Check(issues.OpenCount() == 1, "Generic Ready resolved an observed fault.");
        for (const auto& event : events)
        {
            const auto projection = event.message + event.detail + event.resource;
            Check(projection.find("PRIVATE_WORKER_SENTINEL") == std::string::npos &&
                      projection.find("private approved") == std::string::npos && projection.find("sk-private") == std::string::npos,
                "Worker or dialogue text leaked into a runtime diagnostic.");
        }
        Check(failure->detail.find("PRIVATE_WORKER_SENTINEL") == std::string::npos && failure->evidence.empty(),
            "The durable issue copied worker evidence.");
        backend.fail = false;
        backend.cached = true;
        service.Speak("A cached reply.", {}, 13);
        speech::SpeechServiceTestAccess::TakeNext(service)();
        Check(issues.OpenCount() == 1, "Cached audio resolved the fault.");
        backend.cached = false;
        service.Speak("A fresh reply.", {}, 14);
        speech::SpeechServiceTestAccess::TakeNext(service)();
        Check(issues.OpenCount() == 0 && service.SynthesisHealthSnapshot().state == speech::SynthesisHealth::Available,
            "Fresh matching synthesis did not recover the scoped fault.");
        backend.fail = true;
        service.Speak("A recurrence.", {}, 15);
        speech::SpeechServiceTestAccess::TakeNext(service)();
        Check(issues.OpenCount() == 1 && issues.Find("Voice", "synthesis-failed", origin.profileId, origin.presetId)->occurrences == 2,
            "A new failure episode did not reopen existing history.");
    }
    {
        ReviaSession restarted;
        ReviaSessionTestAccess::LoadSpeechFaultHistory(restarted);
        auto& service = ReviaSessionTestAccess::Speech(restarted);
        speech::SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(service, SpeechSettings(directory.root, backend.port),
            FixtureVoice(directory.root), [&](const auto& event) { ReviaSessionTestAccess::SpeechEvent(restarted, event); });
        Check(service.SynthesisHealthSnapshot().state == speech::SynthesisHealth::Degraded &&
                  service.SynthesisHealthSnapshot().restoredFailure,
            "Restart forgot an unresolved voice fault.");
        for (int index = 0; index < 70; ++index)
            service.UseVoice(FixtureVoice(directory.root, "temporary-" + std::to_string(index)));
        service.UseVoice(FixtureVoice(directory.root));
        Check(service.SynthesisHealthSnapshot().state == speech::SynthesisHealth::Degraded,
            "Bounded projection eviction forgot durable unresolved health.");
    }
}

void TestSafePrivateAndPublicGrounding()
{
    tests::ScopedTestDirectory directory;
    FixtureEnvironment environment(directory.root);
    FaultBackend backend(directory.root);
    messageRouter router;
    aiProfile profile;
    profile.id = "fixture-main";
    profile.displayName = "Revia";
    profile.bMemoryEnabled = false;
    llmSettings llm;
    llm.host = "127.0.0.1";
    llm.port = backend.port;
    llm.modelName = "fixture-main";
    llm.bAutoStartServer = false;
    llm.bAutoMaxTokens = false;
    llm.maxTokens = 256;
    embeddingSettings embedding;
    embedding.bEnabled = false;
    router.ApplyLLMSettings(llm, llm, llm, embedding, profile, true, true);
    conversationContext context;
    agents::TurnCoordinator coordinator;
    speech::SpeechService speech;
    AffectController affect;
    emotion::EmotionRuntime emotions;
    RuntimeEventBus events;
    logger log;
    ConversationRuntime runtime(
        router, context, coordinator, speech, affect, emotions, events, log, [](RuntimeState, const std::string&) {},
        [](const AffectSnapshot&) {}, [] { return actions::CapabilitySettings::InternetAccess{}; },
        [] { return actions::CapabilitySettings::DesktopControl{}; }, {},
        []
        {
            responseFilterSettings filters;
            filters.bAiReviewEnabled = false;
            return filters;
        },
        {});
    speech::SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(
        speech, SpeechSettings(directory.root, backend.port), FixtureVoice(directory.root, "PRIVATE_VOICE_ID"), {});
    Check(runtime.EvaluateTurn("What voice is available?", {}, profile, true).succeeded, "Private grounding did not execute.");
    Check(backend.Posture().find("has not been verified") != std::string::npos, "Assigned voice was presented as verified.");
    speech.Speak("PRIVATE_UTTERANCE", {}, 1);
    speech::SpeechServiceTestAccess::TakeNext(speech)();
    Check(runtime.EvaluateTurn("What voice is available?", {}, profile, true).succeeded, "Fault grounding did not execute.");
    Check(backend.Posture().find("cause is unknown") != std::string::npos &&
              backend.Posture().find("Replies remain available as text") != std::string::npos,
        "Private posture lacked safe observed fault health.");
    identity::RelationshipState person;
    const auto reply = runtime.ReplyPublic("What voice is available?", {}, "A harmless public fixture.", person, profile, true, false);
    Check(reply.succeeded && !reply.text.empty(), "Public fault did not retain text response.");
    const auto posture = backend.Posture();
    Check(posture.find("cause is unknown") != std::string::npos && posture.find("PRIVATE_VOICE_ID") == std::string::npos &&
              posture.find("PRIVATE_UTTERANCE") == std::string::npos && posture.find("PRIVATE_WORKER_SENTINEL") == std::string::npos &&
              posture.find(directory.root.string()) == std::string::npos,
        "Public health omitted the fault or exposed private scope/evidence.");
}

void TestSessionStorageFailureIsBoundedAndNonRecursive()
{
    tests::ScopedTestDirectory directory;
    FixtureEnvironment environment(directory.root);
    FaultBackend backend(directory.root);
    std::filesystem::create_directories(directory.root / "Logs" / "issues.jsonl");
    std::vector<RuntimeEvent> events;
    ReviaSession session;
    session.Events().Subscribe([&](const auto& event) { events.push_back(event); });
    ReviaSessionTestAccess::LoadSpeechFaultHistory(session);
    auto& service = ReviaSessionTestAccess::Speech(session);
    speech::SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(service, SpeechSettings(directory.root, backend.port),
        FixtureVoice(directory.root), [&](const auto& event) { ReviaSessionTestAccess::SpeechEvent(session, event); });
    for (int index = 0; index < 2; ++index)
    {
        service.UseVoice(FixtureVoice(directory.root, "storage-" + std::to_string(index)));
        service.Speak("A harmless storage failure fixture.", {}, 30 + index);
        speech::SpeechServiceTestAccess::TakeNext(service)();
    }
    Check(WarningCount(events, "issue history could not be saved or loaded") == 1,
        "Session diagnostic storage failures did not produce exactly one safe warning.");
    Check(backend.synthesisRequests == 2 && service.SynthesisHealthSnapshot().state == speech::SynthesisHealth::Degraded,
        "Diagnostic storage failure recursively synthesized or erased current health.");
    for (const auto& event : events)
    {
        if (event.kind == RuntimeEventKind::Warning)
            Check(event.message.find(directory.root.string()) == std::string::npos &&
                      event.message.find("PRIVATE_WORKER_SENTINEL") == std::string::npos,
                "A storage warning exposed a path or raw worker failure.");
    }
}

void TestManualVoiceFailurePreservesReplyIntent()
{
    tests::ScopedTestDirectory directory;
    std::filesystem::create_directories(directory.root / "Config");
    FixtureEnvironment environment(directory.root);
    FaultBackend backend(directory.root);
    std::vector<RuntimeEvent> events;
    ReviaSession session;
    auto& service = ReviaSessionTestAccess::Speech(session);
    speech::SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(service, SpeechSettings(directory.root, backend.port),
        FixtureVoice(directory.root), [&](const auto& event) { ReviaSessionTestAccess::SpeechEvent(session, event); });
    const auto subscription = session.Events().Subscribe([&](const auto& event) { events.push_back(event); });
    ReviaSessionTestAccess::SetSpeakingIntent(session, 91);
    const auto result = service.PrepareActiveVoice();
    Check(!result.succeeded, "The manual preparation fixture did not produce its controlled HTTP failure.");
    Check(ReviaSessionTestAccess::SpeakingIntent(session) == 91,
        "A manual voice preparation failure released the active conversational intent.");
    Check(std::any_of(events.begin(), events.end(),
              [](const auto& event) { return event.component == "Voice studio" && event.phase == "Fallback" && event.turnId == 0; }),
        "The manual failure did not have a separate safe Voice studio status.");
    Check(std::none_of(events.begin(), events.end(), [](const auto& event) { return event.component == "Voice"; }),
        "A manual voice status changed ordinary audio activity.");
    ReviaSessionTestAccess::SpeechEvent(session, {"Ready", "Ordinary playback completed."});
    session.Events().Unsubscribe(subscription);
    Check(
        ReviaSessionTestAccess::SpeakingIntent(session) == 0, "Separating manual voice status broke ordinary playback intent completion.");
}

void TestCueStatusProjectsCurrentSelectionAndAudio()
{
    tests::ScopedTestDirectory directory;
    FixtureEnvironment environment(directory.root);
    tests::WriteMinimalWav(directory.root / "reference.wav");
    const auto settings = SpeechSettings(directory.root, 1);
    const auto voice = FixtureVoice(directory.root);
    const std::string profile = "cue-runtime-fixture";
    const auto bank = speech::SystemCueBank::ForVoice(settings.voiceDataPath, profile, voice);
    Check(bank.has_value(), "The cue integration fixture could not key its voice.");
    const auto scratch = bank->ScratchPath(speech::SystemCueKind::Filter);
    tests::WriteMinimalWav(scratch);
    Check(bank->Publish(speech::SystemCueKind::Filter, scratch), "The cue fixture could not publish its valid cache asset.");
    std::vector<RuntimeEvent> events;
    bool switched = false;
    bool staleTerminalPreservedAudio = false;
    ReviaSession session;
    auto& service = ReviaSessionTestAccess::Speech(session);
    service.SetActiveProfile(profile);
    const auto subscription = session.Events().Subscribe([&](const auto& event) { events.push_back(event); });
    speech::SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(service, settings, voice,
        [&](const auto& event)
        {
            ReviaSessionTestAccess::SpeechEvent(session, event);
            if (event.phase == "CuePlaying" && !switched)
            {
                switched = true;
                service.UseVoice(FixtureVoice(directory.root, "voice-B"));
                ReviaSessionTestAccess::SpeechEvent(session, {"CuePlayed", "PRIVATE_CUE_BODY", -1.0, 0, 991});
                const auto lastVoice =
                    std::find_if(events.rbegin(), events.rend(), [](const auto& value) { return value.component == "Voice"; });
                staleTerminalPreservedAudio = service.IsAudioPlaying() && lastVoice != events.rend() && lastVoice->phase == "Speaking" &&
                                              ReviaSessionTestAccess::SpeakingIntent(session) == 91;
            }
        });
    service.RestoreSynthesisFault(profile, voice.id);
    ReviaSessionTestAccess::SetSpeakingIntent(session, 91);
    Check(service.QueueSystemCue(speech::SystemCueKind::Filter), "The valid cached cue did not use the existing queue.");
    speech::SpeechServiceTestAccess::TakeNext(service)();
    const auto play = speech::SpeechServiceTestAccess::TakeCuePlayback(service);
    play([](const auto& path) { return speech::IsPlayableWavFile(path); });
    ReviaSessionTestAccess::SpeechEvent(session, {"CuePrepared", "PRIVATE_CUE_BODY", -1.0, 0, 991});
    session.Events().Unsubscribe(subscription);
    Check(switched && staleTerminalPreservedAudio,
        "A late cache notification cleared the actual audio channel or borrowed the conversational intent.");
    Check(!service.IsAudioPlaying() && ReviaSessionTestAccess::SpeakingIntent(session) == 91,
        "Cue completion did not stop actual audio activity independently of the conversational intent.");
    const auto currentCue =
        std::find_if(events.rbegin(), events.rend(), [](const auto& event) { return event.component == "System cues"; });
    Check(currentCue != events.rend() && currentCue->phase == "Unavailable" && currentCue->turnId == 0,
        "An old CuePrepared event mislabeled the new selected cache or borrowed a reply turn.");
    const auto currentVoice = std::find_if(events.rbegin(), events.rend(), [](const auto& event) { return event.component == "Voice"; });
    Check(currentVoice != events.rend() && currentVoice->phase == "Stopped",
        "The actual cue terminal callback left Runtime audio activity on.");
    Check(service.SynthesisHealthSnapshot(profile, voice.id).state == speech::SynthesisHealth::Degraded,
        "Cached playback cleared an ordinary synthesis fault.");
    for (const auto& event : events)
    {
        Check(event.message.find("PRIVATE_CUE_BODY") == std::string::npos, "The Runtime cue projection copied a private worker body.");
        if (event.component == "System cues")
            Check(event.turnId == 0, "A cue status borrowed a reply turn.");
    }
}

void TestQueuedRecoveryCannotClearLaterRestoration()
{
    tests::ScopedTestDirectory directory;
    FixtureEnvironment environment(directory.root);
    FaultBackend backend(directory.root);
    std::mutex gateMutex;
    std::condition_variable gate;
    bool holdCallback = false;
    bool callbackBlocked = false;
    bool releaseCallback = false;
    ReviaSession session;
    ReviaSessionTestAccess::LoadSpeechFaultHistory(session);
    auto& service = ReviaSessionTestAccess::Speech(session);
    speech::SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(service, SpeechSettings(directory.root, backend.port),
        FixtureVoice(directory.root),
        [&](const auto& event)
        {
            ReviaSessionTestAccess::SpeechEvent(session, event);
            if (event.synthesis && event.phase == "SynthesisHealth" && event.synthesis->selection.presetId == "blocker")
            {
                std::unique_lock lock(gateMutex);
                if (holdCallback)
                {
                    callbackBlocked = true;
                    gate.notify_all();
                    gate.wait(lock, [&] { return releaseCallback; });
                }
            }
        });
    service.Speak("A harmless failure before eviction.", {}, 50);
    speech::SpeechServiceTestAccess::TakeNext(service)();
    const auto origin = service.SynthesisHealthSnapshot().selection;
    for (int index = 0; index < 70; ++index)
        service.UseVoice(FixtureVoice(directory.root, "eviction-" + std::to_string(index)));
    {
        std::lock_guard lock(gateMutex);
        holdCallback = true;
    }
    std::jthread drainer([&] { service.UseVoice(FixtureVoice(directory.root, "blocker")); });
    {
        std::unique_lock lock(gateMutex);
        if (!gate.wait_for(lock, std::chrono::seconds(3), [&] { return callbackBlocked; }))
        {
            releaseCallback = true;
            gate.notify_all();
            lock.unlock();
            drainer.join();
            Check(false, "The restoration fixture did not hold the real notification drainer.");
        }
    }
    service.UseVoice(FixtureVoice(directory.root));
    backend.fail = false;
    service.Speak("A success queued before restoration.", {}, 51);
    speech::SpeechServiceTestAccess::TakeNext(service)();
    const bool successWasQueued = service.SynthesisHealthSnapshot().state == speech::SynthesisHealth::Available;
    {
        std::lock_guard lock(gateMutex);
        releaseCallback = true;
    }
    gate.notify_all();
    drainer.join();
    Check(successWasQueued, "The queued recovery fixture did not execute its pre-restoration success.");
    const auto open = ReviaSessionTestAccess::SpeechIssues(session).Find("Voice", "synthesis-failed", origin.profileId, origin.presetId);
    Check(open && open->status == diagnostics::IssueStatus::Open &&
              service.SynthesisHealthSnapshot().state == speech::SynthesisHealth::Degraded,
        "A queued success cleared an issue after a later restoration invalidated it.");
    service.Speak("A fresh synthesis after restoration.", {}, 52);
    speech::SpeechServiceTestAccess::TakeNext(service)();
    Check(ReviaSessionTestAccess::SpeechIssues(session).OpenCount() == 0,
        "A later qualifying synthesis could not recover the restored issue.");
}
}

void RunSpeechFaultIntegrationTests()
{
    TestManualVoiceFailurePreservesReplyIntent();
    TestCueStatusProjectsCurrentSelectionAndAudio();
    TestQueuedRecoveryCannotClearLaterRestoration();
    TestScopedJournalAndCheckedPersistence();
    TestServiceToSessionHistoryAndRecovery();
    TestSafePrivateAndPublicGrounding();
    TestSessionStorageFailureIsBoundedAndNonRecursive();
    std::cout << "Speech fault runtime, scoped history, recovery and safe grounding tests passed.\n";
}
