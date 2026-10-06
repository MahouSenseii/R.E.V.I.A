#include "speechServiceTestAccess.h"
#include "testSupport.h"

#include <atomic>
#include <condition_variable>
#include <httplib.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <mutex>
#include <thread>

namespace
{
using namespace revia::speech;
using revia::tests::Check;

class Backend
{
  public:
    Backend()
    {
        server.Get("/health", [](const auto&, auto& reply) { reply.set_content("{}", "application/json"); });
        server.Post("/v1/audio/pcm",
            [&](const auto&, auto& reply)
            {
                ++requests;
                if (onRequest)
                    onRequest();
                reply.set_content("RIFF-controlled-fixture", "audio/wav");
            });
        server.Post("/v1/audio/speech",
            [&](const auto& request, auto& reply)
            {
                ++requests;
                outputPath = nlohmann::json::parse(request.body).value("output_path", "");
                revia::tests::WriteMinimalWav(outputPath, 4);
                if (onRequest)
                    onRequest();
                reply.set_content(nlohmann::json{{"succeeded", true}, {"output_path", outputPath}}.dump(), "application/json");
            });
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Admission fixture could not bind loopback.");
        listener = std::jthread([&] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!server.is_running())
        {
            server.stop();
            if (listener.joinable())
                listener.join();
            Check(false, "Admission fixture did not start.");
        }
    }
    ~Backend()
    {
        server.stop();
        if (listener.joinable())
            listener.join();
    }
    int port = 0;
    std::atomic<int> requests = 0;
    std::function<void()> onRequest;
    std::string outputPath;
    httplib::Server server;
    std::jthread listener;
};

void TestCapturedSapiGuardCannotBeReplacedByFreshDefault()
{
    revia::tests::ScopedTestDirectory directory;
    SpeechService service;
    SpeechServiceTestAccess::ConfigureWithoutWorkers(service, directory.root);
    bool oldContext = true;
    service.SetAdmissionGuard([&] { return oldContext; });
    service.Speak("A harmless queued phrase.", {}, 71);
    oldContext = false;
    service.SetAdmissionGuard([] { return true; });
    SpeechServiceTestAccess::TakeNext(service)();
    Check(!service.HasPendingSpeech(), "A revoked queued SAPI phrase survives generation after a fresh default guard replaces it.");
}

void TestDefaultAndFreshAdmissionRemainUsable()
{
    revia::tests::ScopedTestDirectory directory;
    SpeechService service;
    SpeechServiceTestAccess::ConfigureWithoutWorkers(service, directory.root);
    service.Speak("An ordinary unguarded phrase.", {}, 72);
    SpeechServiceTestAccess::TakeNext(service)();
    Check(service.HasPendingSpeech(), "Default passthrough changed ordinary speech readiness.");
    service.StopSpeaking();
    service.SetAdmissionGuard([]() -> bool { throw std::runtime_error("Private fixture payload"); });
    service.Speak("This context cannot be verified.", {}, 73);
    Check(!service.HasPendingSpeech(), "Exception admission queued speech.");
    int checks = 0;
    service.SetAdmissionGuard(
        [&]
        {
            ++checks;
            Check(SpeechServiceTestAccess::CanLockState(service), "Admission callback ran under the Speech state mutex.");
            return true;
        });
    service.Speak("A fresh admitted phrase.", {}, 80);
    SpeechServiceTestAccess::TakeNext(service)();
    Check(checks >= 3 && service.HasPendingSpeech(), "Fresh admission or callback lock discipline failed.");
}

struct QwenFixture
{
    revia::tests::ScopedTestDirectory directory;
    Backend backend;
    VoicePreset voice;
    std::vector<SpeechEvent> events;
    std::function<void(const SpeechEvent&)> onEvent;
    SpeechService service;
    explicit QwenFixture(const bool directPcm = true)
    {
        voice.id = "fixture-voice";
        voice.referenceAudioPath = (directory.root / "reference.wav").string();
        revia::tests::WriteMinimalWav(voice.referenceAudioPath, 4);
        speechSettings settings;
        settings.backend = "Qwen";
        settings.qwenHost = "127.0.0.1";
        settings.qwenPort = backend.port;
        settings.qwenDevices = {"cpu"};
        settings.qwenDevice = "cpu";
        settings.voiceDataPath = directory.root.string();
        settings.bQwenDirectPcm = directPcm;
        SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(service, settings, voice,
            [&](const auto& event)
            {
                events.push_back(event);
                if (onEvent)
                    onEvent(event);
            });
        service.SetActiveProfile("fixture-profile");
        service.UseVoice(voice);
    }
};

void TestRevokedQwenBeforeAndAfterRealRequestDropsDelivery()
{
    QwenFixture fixture;
    std::atomic<bool> current = true;
    fixture.service.SetAdmissionGuard([&] { return current.load(); });
    fixture.service.Speak("A harmless controlled phrase.", {}, 74);
    current = false;
    SpeechServiceTestAccess::TakeNext(fixture.service)();
    Check(fixture.backend.requests == 0 && !fixture.service.HasPendingSpeech(), "Revoked phrase reached Qwen or retained a playback slot.");
    current = true;
    fixture.backend.onRequest = [&] { current = false; };
    fixture.service.Speak("A harmless in-flight phrase.", {}, 75);
    fixture.events.clear();
    SpeechServiceTestAccess::TakeNext(fixture.service)();
    Check(fixture.backend.requests == 1 && !fixture.service.HasPendingSpeech(),
        "Revoked successful HTTP completion reinserted prepared audio.");
    Check(std::none_of(fixture.events.begin(), fixture.events.end(),
              [](const auto& event) { return event.synthesis && event.synthesis->attemptId; }),
        "Revoked completion published terminal synthesis evidence.");
}

void TestRevokedDiskCompletionReleasesTemporaryAudio()
{
    QwenFixture fixture(false);
    bool current = true;
    fixture.service.SetAdmissionGuard([&] { return current; });
    fixture.backend.onRequest = [&] { current = false; };
    fixture.service.Speak("A harmless disk-backed fixture phrase.", {}, 79);
    SpeechServiceTestAccess::TakeNext(fixture.service)();
    Check(fixture.backend.requests == 1 && !fixture.backend.outputPath.empty() && !std::filesystem::exists(fixture.backend.outputPath) &&
              !fixture.service.HasPendingSpeech() && SpeechServiceTestAccess::QueuesCleared(fixture.service),
        "Revoked disk synthesis left an owned audio file or playback reservation.");
}

void TestCachedCueGuardStopsBeforeFakePlayer()
{
    QwenFixture fixture;
    const auto bank = SystemCueBank::ForVoice(fixture.directory.root, "fixture-profile", fixture.voice);
    Check(bank.has_value(), "Controlled cue key missing.");
    std::filesystem::create_directories(bank->Directory());
    const auto scratch = bank->ScratchPath(SystemCueKind::Stopped);
    revia::tests::WriteMinimalWav(scratch, 4);
    Check(bank->Publish(SystemCueKind::Stopped, scratch), "Controlled cue could not be published.");
    fixture.service.UseVoice(fixture.voice);
    bool current = true;
    fixture.service.SetAdmissionGuard([&] { return current; });
    Check(fixture.service.QueueSystemCue(SystemCueKind::Stopped), "Cached-only cue could not queue.");
    SpeechServiceTestAccess::TakeNext(fixture.service)();
    const auto playback = SpeechServiceTestAccess::TakeCuePlayback(fixture.service);
    current = false;
    fixture.service.SetAdmissionGuard([] { return true; });
    int calls = 0;
    playback(
        [&](const auto&)
        {
            ++calls;
            return true;
        });
    Check(calls == 0 && std::filesystem::exists(bank->Clip(SystemCueKind::Stopped)) && !fixture.service.IsAudioPlaying(),
        "Old cached cue reached playback, deleted a persistent asset, or retained the audio flag.");
    current = true;
    fixture.service.SetAdmissionGuard([&] { return current; });
    Check(fixture.service.QueueSystemCue(SystemCueKind::Stopped), "Fresh cached cue could not queue after revocation.");
    SpeechServiceTestAccess::TakeNext(fixture.service)();
    const auto nextPlayback = SpeechServiceTestAccess::TakeCuePlayback(fixture.service);
    const auto start = fixture.events.size();
    nextPlayback(
        [&](const auto&)
        {
            current = false;
            return true;
        });
    Check(!fixture.service.IsAudioPlaying() && fixture.service.SystemCueStatusSnapshot().phase == SystemCuePhase::Cancelled &&
              std::none_of(fixture.events.begin() + static_cast<std::ptrdiff_t>(start), fixture.events.end(),
                  [](const auto& event) { return event.phase == "CuePlaying"; }),
        "Reentrant player revocation published stale playing activity or retained the audio flag.");
}

void TestRevokedActiveQwenPlaybackPublishesOnlyNeutralCleanup()
{
    QwenFixture fixture(false);
    bool current = true;
    fixture.service.SetAdmissionGuard([&] { return current; });
    fixture.service.Speak("PRIVATE_REVOKED_PLAYBACK_SENTINEL", {}, 88);
    SpeechServiceTestAccess::TakeNext(fixture.service)();
    const auto playback = SpeechServiceTestAccess::TakeQwenPlayback(fixture.service);
    std::size_t revokedAt = 0;
    bool trackedPlayback = false;
    fixture.onEvent = [&](const SpeechEvent& event)
    {
        if (event.phase == "Speaking")
        {
            trackedPlayback = event.playbackEnvelope && !event.playbackEnvelope->values.empty();
            revokedAt = fixture.events.size();
            current = false;
            fixture.service.SetAdmissionGuard([] { return true; });
        }
    };
    int started = 0;
    Check(playback([&] { ++started; return true; }), "Revoked playback fell through to another speech backend.");
    Check(started == 1 && trackedPlayback && revokedAt > 0, "Admission was not revoked after the actual tracked playback start.");
    Check(fixture.events.size() == revokedAt + 1, "Revoked playback did not publish exactly one neutral cleanup event.");
    const auto& cleanup = fixture.events.back();
    Check(cleanup.phase == "PlaybackEnded" && cleanup.utteranceId == 88 && cleanup.detail == "Voice audio playback has finished." &&
              cleanup.device.empty() && cleanup.elapsedMilliseconds < 0 && cleanup.queueDepth == 0 && cleanup.timings.empty() &&
              !cleanup.synthesis && !cleanup.playbackEnvelope,
        "Revoked playback retained its mouth track or published stale private speech evidence.");
    Check(!fixture.service.IsAudioPlaying() && !std::filesystem::exists(fixture.backend.outputPath),
        "Revoked active playback retained audio activity or its temporary WAV.");
}

void TestBatchingDoesNotMergeDistinctAdmissionContexts()
{
    QwenFixture fixture;
    fixture.service.SetAdmissionGuard([] { return true; });
    fixture.service.Speak("A first harmless follow-on phrase.", {}, 76, false);
    fixture.service.SetAdmissionGuard([] { return true; });
    fixture.service.Speak("A second harmless follow-on phrase.", {}, 77, false);
    std::size_t count = 9;
    SpeechServiceTestAccess::TakeNextAndCollectBatch(fixture.service, count);
    Check(count == 0 && SpeechServiceTestAccess::PendingAffects(fixture.service).size() == 1,
        "Distinct captured admission scopes were merged into one synthesis batch.");
    fixture.service.StopSpeaking();
    fixture.service.SetAdmissionGuard([] { return true; });
    fixture.service.Speak("A first same-context follow-on phrase.", {}, 78, false);
    fixture.service.Speak("A second same-context follow-on phrase.", {}, 78, false);
    SpeechServiceTestAccess::TakeNextAndCollectBatch(fixture.service, count);
    Check(count == 1, "Same-context batching optimization was lost.");
}

template <typename Service>
void SpeakCaptured(
    Service& service, const std::string& text, const std::uint64_t turn, const std::shared_ptr<const std::function<bool()>>& admission)
{
    if constexpr (requires { service.Speak(text, revia::runtime::AffectSnapshot{}, turn, false, admission); })
        service.Speak(text, {}, turn, false, admission);
    else
        service.Speak(text, {}, turn, false);
}

void TestConcurrentProducerKeepsItsCapturedAdmission()
{
    revia::tests::ScopedTestDirectory directory;
    SpeechService service;
    SpeechServiceTestAccess::ConfigureWithoutWorkers(service, directory.root);
    std::atomic<bool> contextA = true;
    const auto capturedA = std::make_shared<const std::function<bool()>>([&] { return contextA.load(); });
    service.SetAdmissionGuard(*capturedA);
    std::mutex gate;
    std::condition_variable changed;
    bool checked = false;
    bool producerBFinished = false;
    std::atomic<bool> timedOut = false;
    bool initialAdmission = false;
    std::jthread producerA(
        [&]
        {
            initialAdmission = (*capturedA)();
            {
                std::unique_lock lock(gate);
                checked = true;
                changed.notify_all();
                if (!changed.wait_for(lock, std::chrono::seconds(2), [&] { return producerBFinished; }))
                {
                    timedOut = true;
                    return;
                }
            }
            SpeakCaptured(service, "Producer A's obsolete harmless phrase.", 81, capturedA);
        });
    std::jthread producerB(
        [&]
        {
            {
                std::unique_lock lock(gate);
                if (!changed.wait_for(lock, std::chrono::seconds(2), [&] { return checked; }))
                {
                    timedOut = true;
                    return;
                }
            }
            service.SetAdmissionGuard([] { return true; });
            contextA = false;
            service.Speak("Producer B's current harmless phrase.", {}, 82, false);
            {
                std::lock_guard lock(gate);
                producerBFinished = true;
            }
            changed.notify_all();
        });
    producerA.join();
    producerB.join();
    Check(!timedOut && initialAdmission, "Controlled producer boundary did not execute.");
    Check(SpeechServiceTestAccess::PendingAffects(service).size() == 1,
        "Producer B's replacement default admitted producer A's revoked context before enqueue.");
    SpeechServiceTestAccess::TakeNext(service)();
    Check(service.HasPendingSpeech(), "Current producer B lost its ordinary prepared phrase.");
}

void TestExplicitHandlesFailClosedAndPreserveBatchScope()
{
    QwenFixture fixture;
    fixture.service.SetAdmissionGuard([] { return true; });
    SpeakCaptured(fixture.service, "A null explicit handle cannot borrow a default.", 83, {});
    const auto denied = std::make_shared<const std::function<bool()>>([] { return false; });
    SpeakCaptured(fixture.service, "A revoked explicit context cannot borrow a default.", 83, denied);
    Check(!fixture.service.HasPendingSpeech() && fixture.backend.requests == 0,
        "Explicit missing or denied context borrowed a mutable fresh default.");
    const auto captured = std::make_shared<const std::function<bool()>>([] { return true; });
    fixture.service.SetAdmissionGuard([] { return false; });
    SpeakCaptured(fixture.service, "A captured context remains independently admitted.", 84, captured);
    SpeechServiceTestAccess::TakeNext(fixture.service)();
    Check(fixture.backend.requests == 1 && fixture.service.HasPendingSpeech(), "Explicit admitted context was replaced by the default.");
    fixture.service.StopSpeaking();
    SpeakCaptured(fixture.service, "A same-handle first phrase.", 85, captured);
    SpeakCaptured(fixture.service, "A same-handle second phrase.", 85, captured);
    std::size_t companions = 0;
    SpeechServiceTestAccess::TakeNextAndCollectBatch(fixture.service, companions);
    Check(companions == 1, "Explicit same-handle batching optimization was lost.");
    fixture.service.StopSpeaking();
    const auto other = std::make_shared<const std::function<bool()>>([] { return true; });
    SpeakCaptured(fixture.service, "A distinct-handle first phrase.", 86, captured);
    SpeakCaptured(fixture.service, "A distinct-handle second phrase.", 87, other);
    SpeechServiceTestAccess::TakeNextAndCollectBatch(fixture.service, companions);
    Check(companions == 0 && SpeechServiceTestAccess::PendingAffects(fixture.service).size() == 1,
        "Different explicit captured scopes shared a synthesis batch.");
}
} // namespace

void RunSpeechAdmissionTests()
{
    TestCapturedSapiGuardCannotBeReplacedByFreshDefault();
    TestDefaultAndFreshAdmissionRemainUsable();
    TestRevokedQwenBeforeAndAfterRealRequestDropsDelivery();
    TestRevokedDiskCompletionReleasesTemporaryAudio();
    TestCachedCueGuardStopsBeforeFakePlayer();
    TestRevokedActiveQwenPlaybackPublishesOnlyNeutralCleanup();
    TestBatchingDoesNotMergeDistinctAdmissionContexts();
    TestConcurrentProducerKeepsItsCapturedAdmission();
    TestExplicitHandlesFailClosedAndPreserveBatchScope();
    std::cout << "Speech admission tests passed (9 owner fixtures; no physical playback).\n";
}
