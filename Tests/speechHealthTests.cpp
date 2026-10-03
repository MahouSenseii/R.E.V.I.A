#include "speechServiceTestAccess.h"
#include "testSupport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace
{
using namespace revia::speech;
using revia::tests::Check;

class SynthesisBackend
{
  public:
    SynthesisBackend()
    {
        server.Get("/health", [](const auto&, auto& response) { response.set_content("{}", "application/json"); });
        server.Post("/v1/audio/pcm",
            [this](const auto&, auto& response)
            {
                ++requests;
                if (fail.load())
                {
                    response.status = 500;
                    response.set_content(R"({"message":"PRIVATE_SYNTHESIS_SENTINEL"})", "application/json");
                }
                else
                {
                    response.set_header("X-Revia-Audio-Cache-Hit", cached.load() ? "1" : "0");
                    response.set_content(empty.load() ? "" : "RIFF-fixture-audio", "audio/wav");
                }
            });
        server.Post("/v1/audio/pcm-batch",
            [this](const auto&, auto& response)
            {
                ++batchRequests;
                if (batchSuccess.load())
                {
                    response.set_header("X-Revia-Batch-Sizes", "4,4");
                    response.set_header("X-Revia-Audio-Cache-Hit", cached.load() ? "1" : "0");
                    response.set_content("RIFFRIFF", "audio/wav");
                    return;
                }
                if (!declineBatch.load())
                    response.status = 500;
                response.set_content(R"({"message":"PRIVATE_SYNTHESIS_SENTINEL"})", "application/json");
            });
        server.Post("/v1/audio/speech", [](const auto&, auto& response)
            { response.set_content(R"({"succeeded":true,"message":"ok","output_path":"missing-fixture.wav"})", "application/json"); });
        const auto prepare = [](const auto&, auto& response)
        { response.set_content(R"({"succeeded":true,"message":"PRIVATE_SYNTHESIS_SENTINEL"})", "application/json"); };
        server.Post("/prepare-voice", prepare);
        server.Post("/prepare", prepare);
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind the harmless synthesis fixture.");
        thread = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Check(server.is_running(), "The harmless synthesis fixture did not start.");
    }

    ~SynthesisBackend()
    {
        server.stop();
        thread.join();
    }

    int port = 0;
    std::atomic<int> requests = 0;
    std::atomic<int> batchRequests = 0;
    std::atomic<bool> fail = true;
    std::atomic<bool> cached = false;
    std::atomic<bool> empty = false;
    std::atomic<bool> declineBatch = true;
    std::atomic<bool> batchSuccess = false;

  private:
    httplib::Server server;
    std::jthread thread;
};

constexpr const char* safeFailure = "Voice synthesis failed for this reply. The text remains available. The cause is unknown.";

struct OnExit
{
    std::function<void()> action;
    ~OnExit()
    {
        action();
    }
};

struct Fixture
{
    revia::tests::ScopedTestDirectory directory;
    SynthesisBackend backend;
    speechSettings settings;
    VoicePreset voice;
    std::vector<SpeechEvent> events;
    SpeechService service;

    explicit Fixture(SpeechService::EventHandler callback = {})
    {
        settings.backend = "Qwen";
        settings.qwenHost = "127.0.0.1";
        settings.qwenPort = backend.port;
        settings.qwenDevices = {"cpu"};
        settings.qwenDevice = "cpu";
        settings.voiceDataPath = directory.root.string();
        voice.id = "fixture-voice";
        voice.referenceAudioPath = (directory.root / "fixture-reference.wav").string();
        SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(service, settings, voice,
            [this, callback = std::move(callback)](const SpeechEvent& event)
            {
                events.push_back(event);
                if (callback)
                    callback(event);
            });
        service.SetActiveProfile("voice.person");
        service.UseVoice(voice);
    }

    void Render(const std::uint64_t turn = 41)
    {
        service.Speak("A harmless fixture reply.", {}, turn);
        SpeechServiceTestAccess::TakeNext(service)();
    }

    std::function<void(VoiceOperationResult)> Start(const std::uint64_t turn = 41)
    {
        service.Speak("A harmless controlled reply.", {}, turn);
        return SpeechServiceTestAccess::StartNextSynthesis(service);
    }

    std::vector<SynthesisObservation> Terminals() const
    {
        std::vector<SynthesisObservation> result;
        for (const auto& event : events)
            if (event.synthesis && event.synthesis->attemptId != 0)
                result.push_back(*event.synthesis);
        return result;
    }
};

VoiceOperationResult Success(const bool cached = false)
{
    VoiceOperationResult result{true, "Fixture audio.", {}, 1.0};
    result.audioBytes = {'R', 'I', 'F', 'F'};
    result.audioCacheHit = cached;
    return result;
}

VoiceOperationResult Failure()
{
    return {false, "PRIVATE_SYNTHESIS_SENTINEL", {}, 1.0};
}

void CheckSafeEvents(const std::vector<SpeechEvent>& events)
{
    std::uint64_t previous = 0;
    std::set<std::uint64_t> seen;
    for (const auto& event : events)
    {
        Check(event.detail.find("PRIVATE_SYNTHESIS_SENTINEL") == std::string::npos, "Raw worker content escaped in a Speech event.");
        if (!event.synthesis)
            continue;
        Check(event.synthesis->observationId > previous && seen.insert(event.synthesis->observationId).second,
            "Synthesis observations were duplicate or out of order.");
        previous = event.synthesis->observationId;
        Check(event.utteranceId == event.synthesis->utteranceId, "Outer event lost captured turn attribution.");
        if (event.phase == "SynthesisFailed")
            Check(event.detail == safeFailure, "Failure prose was not allowlisted.");
    }
}

void TestFailureIsReportedAtSynthesisCompletionWithoutPlayback()
{
    revia::tests::ScopedTestDirectory directory;
    SynthesisBackend backend;
    speechSettings settings;
    settings.backend = "Qwen";
    settings.qwenHost = "127.0.0.1";
    settings.qwenPort = backend.port;
    settings.qwenDevices = {"cpu"};
    settings.qwenDevice = "cpu";
    settings.voiceDataPath = directory.root.string();
    VoicePreset voice;
    voice.id = "fixture-voice";
    voice.referenceAudioPath = (directory.root / "fixture-reference.wav").string();
    std::vector<SpeechEvent> events;
    SpeechService service;
    SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(
        service, settings, voice, [&](const SpeechEvent& event) { events.push_back(event); });
    service.Speak("A harmless fixture reply.", {}, 41);
    SpeechServiceTestAccess::TakeNext(service)();
    Check(backend.requests == 1, "The completion regression did not execute real queued synthesis.");
    Check(std::any_of(events.begin(), events.end(), [](const SpeechEvent& event) { return event.phase == "SynthesisFailed"; }),
        "Queued synthesis failed without publishing SynthesisFailed at completion before playback.");
}

void TestRealRecoveryPrivacyAndUnusableOutput()
{
    Fixture fixture;
    fixture.Render();
    fixture.Render(42);
    auto observations = fixture.Terminals();
    Check(observations.size() == 2 && observations[0].stateChanged && !observations[1].stateChanged,
        "Repeated failures did not retain one state transition.");
    Check(observations[1].attemptId > observations[0].attemptId && observations[1].failureWatermark == observations[1].attemptId,
        "Repeated failures did not receive distinct start IDs and watermarks.");
    Check(observations[0].selection.profileId == "voice.person" && observations[0].selection.presetId == fixture.voice.id,
        "The captured dotted profile/preset identity was changed.");
    fixture.backend.fail = false;
    fixture.backend.cached = true;
    fixture.Render();
    Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Degraded, "Cached audio resolved the synthesis fault.");
    SpeechServiceTestAccess::NotifyReady(fixture.service);
    Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Degraded, "Ready resolved the synthesis fault.");
    fixture.backend.cached = false;
    fixture.backend.empty = true;
    fixture.Render();
    Check(!fixture.Terminals().back().succeeded && fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Degraded,
        "An empty HTTP200 audio response proved synthesis available.");
    fixture.backend.empty = false;
    fixture.Render();
    Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Available && fixture.Terminals().back().stateChanged,
        "A fresh matching success did not recover synthesis health.");
    fixture.backend.fail = true;
    SpeechServiceTestAccess::SetPlayingTurn(fixture.service, 999);
    const auto adapter = fixture.service.RenderAdapterSpeech("A harmless adapter reply.");
    Check(!adapter.succeeded && adapter.message == safeFailure && fixture.Terminals().back().utteranceId == 0,
        "Adapter failure leaked content or borrowed an unrelated playing turn.");
    Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Degraded, "A new failure did not reopen recovered health.");
    CheckSafeEvents(fixture.events);
    Check(fixture.backend.requests == 6, "Fault reporting caused recursive synthesis or retries.");
    Check(SpeechServiceTestAccess::SynthesisException().message == safeFailure, "Exception text escaped classification.");
    {
        auto diskSettings = fixture.settings;
        diskSettings.bQwenDirectPcm = false;
        SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(fixture.service, diskSettings, fixture.voice, {});
        fixture.service.RestoreSynthesisFault("voice.person", fixture.voice.id);
        fixture.Render();
        Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Degraded,
            "A missing requested disk artifact proved synthesis available.");
    }
}

void TestWatermarksAndRestart()
{
    Fixture fixture;
    SpeechServiceTestAccess::SetNextSequence(fixture.service, 1000);
    auto earlierSuccess = fixture.Start();
    auto laterFailure = fixture.Start();
    laterFailure(SpeechServiceTestAccess::SynthesisException());
    earlierSuccess(Success());
    auto observations = fixture.Terminals();
    Check(observations[0].attemptId == 2 && observations[0].sequence == 1001 && observations[0].failureWatermark == 2,
        "Attempt start ID was replaced with queue sequence.");
    Check(observations[1].state == SynthesisHealth::Degraded && !observations[1].stateChanged,
        "An already-started success resolved a newer failure.");
    fixture.Start()(Success());
    Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Available, "Fresh success did not recover.");
    auto restoringSuccess = fixture.Start();
    fixture.service.RestoreSynthesisFault("voice.person", fixture.voice.id);
    restoringSuccess(Success());
    const auto restored = fixture.service.SynthesisHealthSnapshot();
    Check(restored.state == SynthesisHealth::Degraded && restored.restoredFailure && restored.failureWatermark == 4,
        "In-flight success erased a restored fault.");
    fixture.Start()(Success());
    Check(!fixture.service.SynthesisHealthSnapshot().restoredFailure &&
              fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Available,
        "Fresh verification did not clear restored health.");
    const auto prior = fixture.Terminals().back();
    SpeechServiceTestAccess::RestartHealthWithoutWorkers(fixture.service);
    Check(
        fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Unverified, "Service restart retained verified availability.");
    fixture.Start()(Failure());
    const auto next = fixture.Terminals().back();
    Check(next.attemptId > prior.attemptId && next.observationId > prior.observationId,
        "Restart reset correlation counters used by Runtime's delivery gate.");
    fixture.service.SetActiveProfile("other");
    Check(fixture.service.SynthesisHealthSnapshot("voice.person", fixture.voice.id).state == SynthesisHealth::Degraded,
        "Scoped query lost fault state while another profile was selected.");
    CheckSafeEvents(fixture.events);
}

void TestStaleCancellationAndSelections()
{
    for (int action = 0; action < 3; ++action)
    {
        Fixture fixture;
        auto completion = fixture.Start();
        if (action == 0)
            fixture.service.StopSpeaking();
        if (action == 1)
            fixture.service.SetEnabled(false);
        if (action == 2)
        {
            fixture.service.RequestVoiceShutdown();
        }
        completion(Failure());
        Check(fixture.Terminals().empty(), "Cancelled, muted or shutdown-latched work created a synthesis fault.");
        if (action == 2)
            fixture.service.Shutdown();
        Check(SpeechServiceTestAccess::QueuesCleared(fixture.service), "Completed cancellation reinserted prepared audio.");
        if (action == 2)
        {
            fixture.service.SetEnabled(true);
            Check(!fixture.service.IsEnabled(), "Output setting reopened shutdown synthesis.");
        }
    }
    Fixture fixture;
    fixture.Start()(Failure());
    auto oldA = fixture.Start();
    fixture.service.SetActiveProfile("B");
    fixture.service.UseVoice(fixture.voice);
    fixture.service.SetActiveProfile("voice.person");
    fixture.service.UseVoice(fixture.voice);
    Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Degraded, "A->B->A erased the retained A fault.");
    const auto before = fixture.Terminals().size();
    oldA(Success());
    Check(fixture.Terminals().size() == before && fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Degraded,
        "Old A epoch success mutated the newly selected A.");
    auto oldVoice = fixture.Start();
    fixture.service.UseVoice(fixture.voice);
    oldVoice(Failure());
    Check(fixture.Terminals().size() == before, "Replaced voice epoch failure was attributed to the new selection.");
    Check(fixture.service.HasPendingSpeech(), "Changing profile cancelled existing queued/prepared audio.");
}

void TestPreparationAndBankCannotRecover()
{
    Fixture fixture;
    fixture.Render();
    const auto count = fixture.Terminals().size();
    Check(fixture.service.PrepareActiveVoice().succeeded, "Controlled voice preparation failed.");
    Check(fixture.Terminals().size() == count && fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Degraded,
        "Voice preparation proved ordinary synthesis available.");
    const auto bank = fixture.directory.root / fixture.voice.id / "vocalizations";
    std::filesystem::create_directories(bank);
    const auto clip = bank / "soft-laugh-1.wav";
    revia::tests::WriteMinimalWav(clip);
    fixture.service.UseVoice(fixture.voice);
    fixture.service.Speak("*chuckles*", {}, 43);
    SpeechServiceTestAccess::TakeNext(fixture.service)();
    Check(SpeechServiceTestAccess::HasPreparedClip(fixture.service, clip) && fixture.Terminals().size() == count &&
              fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Degraded,
        "A prepared bank clip cleared ordinary synthesis health.");
    CheckSafeEvents(fixture.events);
}

void TestScopeCapAndReentrantRestoration()
{
    Fixture fixture;
    bool historicalFault = false;
    SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(fixture.service, fixture.settings, fixture.voice,
        [&](const SpeechEvent& event)
        {
            fixture.events.push_back(event);
            if (historicalFault && event.phase == "SynthesisHealth" && event.synthesis->selection.profileId == "voice.person" &&
                event.synthesis->selection.presetId == fixture.voice.id)
            {
                fixture.service.RestoreSynthesisFault("voice.person", fixture.voice.id);
                Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Degraded,
                    "Reentrant restoration did not precede snapshot projection.");
            }
        });
    fixture.events.clear();
    fixture.Start()(Failure());
    historicalFault = true;
    for (int index = 0; index < 65; ++index)
    {
        auto voice = fixture.voice;
        voice.id = "fixture-voice-" + std::to_string(index);
        fixture.service.UseVoice(voice);
        Check(SpeechServiceTestAccess::HealthScopeCount(fixture.service) <= 64, "Live scope projection exceeded its cap.");
    }
    const auto count = SpeechServiceTestAccess::HealthScopeCount(fixture.service);
    Check(fixture.service.SynthesisHealthSnapshot("voice.person", fixture.voice.id).state == SynthesisHealth::Unverified &&
              SpeechServiceTestAccess::HealthScopeCount(fixture.service) == count,
        "Evicted scope query inserted or fabricated state.");
    fixture.service.UseVoice(fixture.voice);
    Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Degraded &&
              fixture.service.SynthesisHealthSnapshot().restoredFailure,
        "Durable scope restoration did not recover an evicted unresolved fault.");
    CheckSafeEvents(fixture.events);
}

void TestBatchDeclineAndOperationalFailure()
{
    for (const bool decline : {true, false})
    {
        Fixture fixture;
        fixture.backend.declineBatch = decline;
        fixture.backend.fail = false;
        fixture.service.Speak("A harmless first phrase.", {}, 41, false);
        fixture.service.Speak("A harmless second phrase.", {}, 41, false);
        SpeechServiceTestAccess::SynthesizeQueuedBatch(fixture.service);
        const auto observations = fixture.Terminals();
        Check(fixture.backend.batchRequests == 1 && fixture.backend.requests == 2,
            "Batch fallback changed its bounded per-phrase request behavior.");
        Check(observations.size() == (decline ? 2 : 3), "Batch decline/failure classification was incorrect.");
        Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Available,
            "Per-phrase fallback did not provide fresh recovery evidence.");
        if (!decline)
            Check(!observations.front().succeeded && observations.front().stateChanged,
                "Operational batch failure did not become health before fallback.");
        CheckSafeEvents(fixture.events);
    }
    Fixture fixture;
    fixture.Render();
    fixture.backend.batchSuccess = true;
    fixture.backend.cached = true;
    const auto batch = [&]
    {
        fixture.service.Speak("A harmless first phrase.", {}, 41, false);
        fixture.service.Speak("A harmless second phrase.", {}, 41, false);
        SpeechServiceTestAccess::SynthesizeQueuedBatch(fixture.service);
    };
    batch();
    Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Degraded && !fixture.Terminals().back().fresh,
        "Cached batched audio resolved a synthesis fault.");
    fixture.backend.cached = false;
    batch();
    Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Available && fixture.backend.requests == 1 &&
              fixture.backend.batchRequests == 2,
        "Successful batches lost optimization or fresh recovery evidence.");
    const auto observations = fixture.Terminals();
    Check(observations[3].attemptId == observations[4].attemptId && observations[3].sequence != observations[4].sequence &&
              observations[3].stateChanged && !observations[4].stateChanged,
        "One batch attempt lost per-phrase correlation.");
    CheckSafeEvents(fixture.events);
}

void TestBoundedNotificationBackpressureAndCancellation()
{
    for (const bool cancel : {false, true})
    {
        revia::tests::ScopedTestDirectory directory;
        SpeechService service;
        speechSettings settings;
        settings.backend = "Qwen";
        settings.voiceDataPath = directory.root.string();
        VoicePreset voice;
        voice.id = "fixture-voice";
        std::mutex gate;
        std::condition_variable ready;
        bool entered = false;
        bool release = false;
        std::vector<SpeechEvent> events;
        SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(service, settings, voice,
            [&](const SpeechEvent& event)
            {
                if (!event.synthesis)
                    return;
                events.push_back(event);
                if (event.phase == "SynthesisFailed" && !entered)
                {
                    std::unique_lock lock(gate);
                    entered = true;
                    ready.notify_all();
                    ready.wait(lock, [&] { return release; });
                }
            });
        const auto start = [&]
        {
            service.Speak("A bounded notification fixture.", {}, 41);
            return SpeechServiceTestAccess::StartNextSynthesis(service);
        };
        auto first = start();
        std::jthread drainer([&] { first(Failure()); });
        OnExit unblock{[&]
            {
                {
                    std::lock_guard lock(gate);
                    release = true;
                }
                ready.notify_all();
            }};
        {
            std::unique_lock lock(gate);
            Check(ready.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }), "Slow callback did not start.");
        }
        for (int index = 1; index <= 128; ++index)
            start()(index % 2 == 0 ? Failure() : Success());
        auto last = start();
        std::atomic<bool> producerStarted = false;
        std::atomic<bool> returned = false;
        std::jthread producer(
            [&]
            {
                producerStarted = true;
                last(Success());
                returned = true;
            });
        const auto producerDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!producerStarted.load() && std::chrono::steady_clock::now() < producerDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const bool bounded = !returned.load() && SpeechServiceTestAccess::HealthNotificationCount(service) == 128;
        if (cancel)
            service.StopSpeaking();
        bool cancellationReturned = true;
        if (cancel)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!returned.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            cancellationReturned = returned.load();
        }
        {
            std::lock_guard lock(gate);
            release = true;
        }
        ready.notify_all();
        drainer.join();
        producer.join();
        Check(bounded, "Terminal notification saturation did not apply bounded backpressure.");
        Check(cancellationReturned, "Cancellation did not release notification backpressure.");
        if (cancel)
            Check(SpeechServiceTestAccess::QueuesCleared(service), "Cancelled waiting producer reinserted prepared audio.");
        std::size_t terminals = 0;
        for (const auto& event : events)
            if (event.synthesis && event.synthesis->attemptId != 0)
                ++terminals;
        Check(terminals == (cancel ? 129 : 130), "Backpressure lost accepted observations or admitted cancelled ones.");
        CheckSafeEvents(events);
    }
}

void TestQueuedSuccessThenRestoration()
{
    revia::tests::ScopedTestDirectory directory;
    SpeechService service;
    speechSettings settings;
    settings.backend = "Qwen";
    settings.voiceDataPath = directory.root.string();
    VoicePreset voice;
    voice.id = "fixture-voice";
    std::vector<SpeechEvent> events;
    std::mutex gate;
    std::condition_variable ready;
    bool blocking = false;
    bool entered = false;
    bool release = false;
    SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(service, settings, voice,
        [&](const SpeechEvent& event)
        {
            if (!event.synthesis)
                return;
            events.push_back(event);
            if (event.phase == "SynthesisHealth" && blocking && !entered)
            {
                std::unique_lock lock(gate);
                entered = true;
                ready.notify_all();
                ready.wait(lock, [&] { return release; });
            }
        });
    service.Speak("A harmless success.", {}, 41);
    auto completion = SpeechServiceTestAccess::StartNextSynthesis(service);
    blocking = true;
    std::jthread drainer([&] { service.SetEnabled(true); });
    OnExit unblock{[&]
        {
            {
                std::lock_guard lock(gate);
                release = true;
            }
            ready.notify_all();
        }};
    {
        std::unique_lock lock(gate);
        Check(ready.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }), "Queued success fixture did not block dispatch.");
    }
    completion(Success());
    service.RestoreSynthesisFault("", voice.id);
    const auto snapshot = service.SynthesisHealthSnapshot("", voice.id);
    {
        std::lock_guard lock(gate);
        release = true;
    }
    ready.notify_all();
    drainer.join();
    const auto found = std::find_if(events.begin(), events.end(), [](const auto& event) { return event.phase == "SynthesisAvailable"; });
    Check(found != events.end(), "Queued Available observation was lost.");
    const auto success = found->synthesis;
    Check(success && success->state == SynthesisHealth::Available && snapshot.state == SynthesisHealth::Degraded &&
              snapshot.failureWatermark >= success->attemptId,
        "Older Available evidence survived a newer restoration watermark.");
    CheckSafeEvents(events);
}

void TestBatchCompanionsRespectCapturedSelection()
{
    for (const bool changeProfile : {false, true})
    {
        Fixture fixture;
        fixture.service.Speak("A harmless older phrase.", {}, 51, false);
        const auto oldSelection = fixture.service.SynthesisHealthSnapshot().selection;
        if (changeProfile)
            fixture.service.SetActiveProfile("other.person");
        fixture.service.UseVoice(fixture.voice);
        const auto currentSelection = fixture.service.SynthesisHealthSnapshot().selection;
        Check(currentSelection.epoch != oldSelection.epoch && currentSelection.presetId == oldSelection.presetId,
            "The batch selection fixture did not reselect the same preset.");
        fixture.service.Speak("A harmless current phrase.", {}, 52, false);
        std::size_t companions = 0;
        auto completeOlder = SpeechServiceTestAccess::TakeNextAndCollectBatch(fixture.service, companions);
        Check(companions == 0 && SpeechServiceTestAccess::PendingAffects(fixture.service).size() == 1,
            "Batch collection consumed the current phrase across a captured selection boundary.");
        completeOlder();
        Check(fixture.Terminals().empty(), "An older selection failure changed current synthesis health.");
        SpeechServiceTestAccess::TakeNext(fixture.service)();
        const auto observations = fixture.Terminals();
        Check(fixture.backend.requests == 2 && fixture.backend.batchRequests == 0 && observations.size() == 1,
            "The isolated current phrase did not execute its own ordinary synthesis attempt.");
        Check(observations.front().utteranceId == 52 && observations.front().selection.profileId == currentSelection.profileId &&
                  observations.front().selection.presetId == currentSelection.presetId &&
                  observations.front().selection.epoch == currentSelection.epoch &&
                  fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Degraded,
            "Current selection synthesis failure disappeared behind a stale batch leader.");
        CheckSafeEvents(fixture.events);
    }
}

void TestSameSelectionBatchOptimization()
{
    Fixture fixture;
    fixture.backend.batchSuccess = true;
    fixture.service.Speak("A harmless first phrase.", {}, 53, false);
    fixture.service.Speak("A harmless second phrase.", {}, 53, false);
    std::size_t companions = 0;
    auto complete = SpeechServiceTestAccess::TakeNextAndCollectBatch(fixture.service, companions);
    Check(companions == 1 && SpeechServiceTestAccess::PendingAffects(fixture.service).empty(),
        "Same-selection queued phrases no longer collect into a batch.");
    complete();
    const auto observations = fixture.Terminals();
    Check(fixture.backend.batchRequests == 1 && fixture.backend.requests == 0 && observations.size() == 2 &&
              observations[0].attemptId == observations[1].attemptId &&
              fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Available,
        "Same-selection collection lost the single-call batch optimization.");
    CheckSafeEvents(fixture.events);
}
}

void RunSpeechHealthTests()
{
    TestFailureIsReportedAtSynthesisCompletionWithoutPlayback();
    TestRealRecoveryPrivacyAndUnusableOutput();
    TestWatermarksAndRestart();
    TestStaleCancellationAndSelections();
    TestPreparationAndBankCannotRecover();
    TestScopeCapAndReentrantRestoration();
    TestBatchDeclineAndOperationalFailure();
    TestBoundedNotificationBackpressureAndCancellation();
    TestQueuedSuccessThenRestoration();
    TestBatchCompanionsRespectCapturedSelection();
    TestSameSelectionBatchOptimization();
    std::cout << "Speech synthesis health completion tests passed.\n";
}
