#include "speechServiceTestAccess.h"
#include "systemCueTestAccess.h"
#include "testSupport.h"
#include "Speech/systemCue.h"
#include "Speech/systemCueBank.h"
#include "Speech/vocalization.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <thread>

namespace
{
using namespace revia::speech;
using revia::tests::Check;
constexpr const char* Sentinel = "PRIVATE_MANUAL_DIAGNOSTIC_SENTINEL";

template <class Predicate> bool WaitUntil(Predicate predicate, const int milliseconds = 2000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!predicate() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return predicate();
}

struct Gate
{
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    void Block()
    {
        std::unique_lock lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return released; });
    }
    bool Entered()
    {
        std::lock_guard lock(mutex);
        return entered;
    }
    void Release()
    {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }
};

struct OnExit
{
    std::function<void()> action;
    ~OnExit()
    {
        action();
    }
};

class ReservedServer : public httplib::Server
{
  public:
    ReservedServer()
    {
        set_socket_options([](const socket_t socket)
            {
                const int enabled = 1;
                if (setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                        reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0)
                {
                    closesocket(socket);
                    throw std::runtime_error("Could not exclusively reserve a cue fixture socket.");
                }
            });
    }
    ~ReservedServer()
    {
        ReleaseReservation();
    }
    void ReleaseReservation()
    {
        assert(!is_running());
        // The pinned httplib stop/destructor leave pre-listen sockets open.
        const auto socket = svr_sock_.exchange(INVALID_SOCKET);
        if (socket != INVALID_SOCKET)
            closesocket(socket);
        stop();
    }
};

using PortBinder = std::function<int(ReservedServer&, int)>;

int BindCuePair(ReservedServer& server, ReservedServer& design, const PortBinder& bind = {}, const int attempts = 32)
{
    int port = -1;
    std::string stage = "no attempts";
    bool reserved = false;
    OnExit rollback{[&]
        {
            if (!reserved)
            {
                server.ReleaseReservation();
                design.ReleaseReservation();
            }
        }};
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        server.ReleaseReservation();
        design.ReleaseReservation();
        port = bind ? bind(server, attempt) : server.bind_to_any_port("127.0.0.1");
        if (port <= 0)
            stage = "ordinary bind";
        else if (port > 65503)
            stage = "design port range";
        else if (!design.bind_to_port("127.0.0.1", port + 32))
            stage = "design bind";
        else
        {
            reserved = true;
            return port;
        }
    }
    throw std::runtime_error("Could not bind cue fixture pair after " + std::to_string(attempts) +
                             " attempts; last base=" + std::to_string(port) + "; stage=" + stage + ".");
}

class Backend
{
  public:
    explicit Backend(const PortBinder& bind = {})
    {
        const auto health = [](const auto&, auto& response) { response.set_content("{}", "application/json"); };
        server.Get("/health", health);
        design.Get("/health", health);
        const auto prepare = [&](const auto&, auto& response)
        {
            if (blockPrepare)
                preparationGate.Block();
            response.status = failManual ? 500 : 200;
            response.set_content(nlohmann::json{{"succeeded", !failManual}, {"message", Sentinel}, {"device_name", Sentinel},
                                     {"device", "cpu"}, {"dtype", "float16"}, {"backend", "low_latency"}, {"attention_backend", "auto"},
                                     {"input_mode", "complete"}, {"low_latency_detail", Sentinel}, {"sample_rate", 24000}}
                                     .dump(),
                "application/json");
        };
        server.Post("/prepare-voice", prepare);
        server.Post("/prepare", prepare);
        server.Post("/v1/audio/pcm",
            [&](const auto& request, auto& response)
            {
                const auto text = nlohmann::json::parse(request.body).value("text", "");
                {
                    std::lock_guard lock(mutex);
                    order.push_back("ordinary:" + text);
                }
                ++ordinaryRequests;
                if (blockOrdinary)
                    ordinaryGate.Block();
                response.set_header("X-Revia-Audio-Cache-Hit", cached ? "1" : "0");
                response.set_content("RIFF-harmless-ordinary-fixture", "audio/wav");
            });
        server.Post("/v1/audio/speech",
            [&](const auto& request, auto& response)
            {
                const auto body = nlohmann::json::parse(request.body);
                const auto text = body.value("text", "");
                const auto& catalog = ApprovedSystemCues();
                const bool cue = std::any_of(catalog.begin(), catalog.end(), [&](const auto& entry) { return entry.phrase == text; });
                if (!cue)
                {
                    response.status = invalidPreview ? 200 : 500;
                    response.set_content(nlohmann::json{{"succeeded", invalidPreview.load()}, {"message", Sentinel},
                                             {"output_path", Sentinel}, {"device_name", Sentinel}}
                                             .dump(),
                        "application/json");
                    return;
                }
                const int number = ++cueRequests;
                {
                    std::lock_guard lock(mutex);
                    cueTexts.push_back(text);
                    order.push_back("cue:" + text);
                }
                if (blockCue && number == 1)
                    cueGate.Block();
                if (failCueAt > 0 && number >= failCueAt)
                {
                    response.status = 500;
                    response.set_content(nlohmann::json{{"succeeded", false}, {"message", Sentinel}}.dump(), "application/json");
                    return;
                }
                const auto output = body.value("output_path", "");
                revia::tests::WriteMinimalWav(output, invalidCue ? 0 : 4);
                response.set_content(
                    nlohmann::json{{"succeeded", true}, {"message", Sentinel}, {"output_path", Sentinel}}.dump(), "application/json");
            });
        design.Post("/v1/voice-design",
            [&](const auto& request, auto& response)
            {
                const auto body = nlohmann::json::parse(request.body);
                if (!failManual)
                    revia::tests::WriteMinimalWav(body.value("output_path", ""), designSamples);
                response.status = failManual ? 500 : 200;
                response.set_content(
                    nlohmann::json{{"succeeded", !failManual}, {"message", Sentinel}, {"device_name", Sentinel}, {"output_path", Sentinel},
                        {"device", "cpu"}, {"dtype", "float16"}, {"backend", "low_latency"}, {"attention_backend", "auto"},
                        {"input_mode", "complete"}, {"sample_rate", 24000}, {"low_latency_detail", Sentinel}}
                        .dump(),
                    "application/json");
            });
        design.Post("/v1/vocalizations",
            [](const auto&, auto& response)
            {
                response.status = 500;
                response.set_content(
                    nlohmann::json{{"succeeded", false}, {"message", Sentinel}, {"device_name", Sentinel}}.dump(), "application/json");
            });
        server.new_task_queue = [] { return new httplib::ThreadPool(4); };
        port = BindCuePair(server, design, bind);
        thread = std::jthread([&] { server.listen_after_bind(); });
        designThread = std::jthread([&] { design.listen_after_bind(); });
        Check(WaitUntil([&] { return server.is_running() && design.is_running(); }), "Cue fixture pair did not start.");
    }
    ~Backend()
    {
        Release();
        server.stop();
        design.stop();
        thread.join();
        designThread.join();
    }
    void Release()
    {
        cueGate.Release();
        ordinaryGate.Release();
        preparationGate.Release();
    }
    std::vector<std::string> Order()
    {
        std::lock_guard lock(mutex);
        return order;
    }
    std::vector<std::string> CueTexts()
    {
        std::lock_guard lock(mutex);
        return cueTexts;
    }
    int port = 0;
    std::atomic<int> cueRequests = 0;
    std::atomic<int> ordinaryRequests = 0;
    std::atomic<int> failCueAt = 0;
    std::atomic<bool> invalidCue = false;
    std::atomic<bool> blockCue = false;
    std::atomic<bool> blockOrdinary = false;
    std::atomic<bool> blockPrepare = false;
    std::atomic<bool> failManual = true;
    std::atomic<bool> invalidPreview = false;
    std::atomic<bool> cached = false;
    std::atomic<int> designSamples = 4;
    Gate cueGate, ordinaryGate, preparationGate;

  private:
    ReservedServer server, design;
    std::jthread thread, designThread;
    std::mutex mutex;
    std::vector<std::string> order, cueTexts;
};

struct Fixture
{
    revia::tests::ScopedTestDirectory directory;
    Backend backend;
    speechSettings settings;
    VoicePreset voice;
    std::mutex eventMutex;
    std::vector<SpeechEvent> events;
    SpeechService service;
    explicit Fixture(SpeechService::EventHandler callback = {}, const PortBinder& bind = {}) : backend(bind)
    {
        settings.backend = "Qwen";
        settings.qwenHost = "127.0.0.1";
        settings.qwenPort = backend.port;
        settings.qwenDevices = {"cpu"};
        settings.qwenDevice = "cpu";
        settings.voiceDataPath = directory.root.string();
        voice.id = "fixture-voice";
        voice.name = "Harmless fixture";
        voice.description = "Harmless voice description.";
        voice.referenceText = "Harmless reference.";
        voice.referenceAudioPath = (directory.root / "reference.wav").string();
        revia::tests::WriteMinimalWav(voice.referenceAudioPath);
        VoicePresetStore store(directory.root);
        std::string error;
        Check(store.Save(voice, error), "Could not save generated fixture voice.");
        SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(service, settings, voice,
            [this, callback = std::move(callback)](const auto& event)
            {
                {
                    std::lock_guard lock(eventMutex);
                    events.push_back(event);
                }
                if (callback)
                    callback(event);
            });
        service.SetActiveProfile("cue.person");
        service.UseVoice(voice);
    }
    std::vector<SpeechEvent> Events()
    {
        std::lock_guard lock(eventMutex);
        return events;
    }
    SystemCueBank Bank(const unsigned version = SystemCuePhraseVersion)
    {
        const auto bank = SystemCueBank::ForVoice(directory.root, "cue.person", voice, version);
        Check(bank.has_value(), "Generated fixture reference did not yield a cue key.");
        return *bank;
    }
    void Fresh()
    {
        service.Speak("Harmless ordinary reply.", {}, 73);
        SpeechServiceTestAccess::TakeNext(service)();
        SpeechServiceTestAccess::ConsumePreparedWithoutPlayback(service);
    }
    void FillCache()
    {
        auto bank = Bank();
        for (const auto& cue : ApprovedSystemCues())
        {
            const auto scratch = bank.ScratchPath(cue.kind);
            revia::tests::WriteMinimalWav(scratch);
            Check(bank.Publish(cue.kind, scratch), "Could not publish controlled cached clip.");
        }
        service.UseVoice(voice);
    }
};

void TestCuePairReservation()
{
    ReservedServer original, occupied;
    const int blockedBase = BindCuePair(original, occupied);
    original.ReleaseReservation();
    int attempts = 0;
    Fixture fixture({}, [&](ReservedServer& server, const int attempt)
        {
            ++attempts;
            if (attempt == 0)
                return server.bind_to_port("127.0.0.1", blockedBase) ? blockedBase : -1;
            return server.bind_to_any_port("127.0.0.1");
        });
    Check(attempts >= 2 && fixture.backend.port != blockedBase, "Cue fixture did not retry an occupied design port.");
    ReservedServer released;
    Check(released.bind_to_port("127.0.0.1", blockedBase), "Failed cue pair leaked its ordinary reservation.");
    released.ReleaseReservation();
    fixture.backend.failManual = false;
    const auto scratch = fixture.Bank().ScratchPath(SystemCueKind::Error);
    Check(SpeechServiceTestAccess::VoicePool(fixture.service).TrySynthesizeSystemCue("Error.", fixture.voice, scratch.string()).succeeded &&
              fixture.backend.cueRequests == 1,
        "Recovered cue pair did not route real synthesis to its ordinary port.");
    Check(fixture.service.CreateVoicePreset("Reserved pair", "Safe description", "Safe reference", "English").succeeded,
        "Recovered cue pair did not route real voice design to its base plus 32 port.");

    ReservedServer server, design;
    int rejectedPort = 0;
    const int recovered = BindCuePair(server, design, [&](ReservedServer& candidate, const int attempt)
        {
            if (attempt == 0)
            {
                rejectedPort = candidate.bind_to_any_port("127.0.0.1");
                Check(rejectedPort > 0, "Could not reserve upper-bound control socket.");
                // Exercise the range guard without requiring any particular high OS port to be free.
                return 65504;
            }
            return candidate.bind_to_any_port("127.0.0.1");
        });
    Check(recovered > 0 && recovered <= 65503, "Cue fixture accepted an overflowing design port.");
    server.ReleaseReservation();
    design.ReleaseReservation();
    Check(released.bind_to_port("127.0.0.1", rejectedPort), "Out-of-range cue pair leaked its ordinary reservation.");
    released.ReleaseReservation();

    int exhausted = 0;
    std::string failure;
    try
    {
        BindCuePair(server, design, [&](ReservedServer& candidate, const int)
            {
                ++exhausted;
                Check(candidate.bind_to_port("127.0.0.1", blockedBase), "Exhaustion retry leaked its ordinary port.");
                return blockedBase;
            }, 3);
    }
    catch (const std::runtime_error& error)
    {
        failure = error.what();
    }
    Check(exhausted == 3 && failure.find("after 3 attempts") != std::string::npos &&
              failure.find("last base=" + std::to_string(blockedBase)) != std::string::npos &&
              failure.find("stage=design bind") != std::string::npos,
        "Cue pair exhaustion lost its bounded attempts or allocation diagnostic.");
    Check(released.bind_to_port("127.0.0.1", blockedBase), "Exhausted cue pair leaked its ordinary reservation.");
    released.ReleaseReservation();
    failure.clear();
    try
    {
        BindCuePair(server, design, [&](ReservedServer& candidate, const int) -> int
            {
                Check(candidate.bind_to_port("127.0.0.1", blockedBase), "Could not reserve exception control socket.");
                throw std::runtime_error("controlled allocation interruption");
            });
    }
    catch (const std::runtime_error& error)
    {
        failure = error.what();
    }
    Check(failure == "controlled allocation interruption" && released.bind_to_port("127.0.0.1", blockedBase),
        "Interrupted cue pair lost its exception or leaked its ordinary reservation.");
    released.ReleaseReservation();
    occupied.ReleaseReservation();
    Check(server.bind_to_port("127.0.0.1", blockedBase) && design.bind_to_port("127.0.0.1", blockedBase + 32),
        "Released cue pair was not reusable after bounded exhaustion.");
}

void CheckPrivateAbsent(const VoiceOperationResult& result)
{
    const std::string text = result.message + result.outputPath + result.device + result.deviceName + result.dtype + result.workerId +
                             result.backend + result.backendDetail + result.attentionBackend + result.inputMode;
    Check(text.find(Sentinel) == std::string::npos, "Manual returned result exposed worker payload.");
}

void TestManualOperationsRetainOrigin()
{
    Fixture fixture;
    SpeechServiceTestAccess::SetPlayingTurn(fixture.service, 991);
    const auto result = fixture.service.PrepareActiveVoice();
    const auto events = fixture.Events();
    const auto failure = std::find_if(events.begin(), events.end(), [](const auto& event) { return event.phase == "ManualFallback"; });
    Check(!result.succeeded && failure != events.end() && failure->utteranceId == 0,
        "Manual voice HTTP failure borrowed the active reply origin or legacy completion phase.");
    for (const auto& event : events)
        if (event.phase.starts_with("Manual"))
            Check(event.utteranceId == 0 && !event.synthesis, "Manual voice event carried an active turn or ordinary health observation.");
}

void TestManualDiagnostics()
{
    Fixture fixture;
    SpeechServiceTestAccess::SetPlayingTurn(fixture.service, 992);
    const auto prepared = fixture.service.PrepareActiveVoice();
    const auto designed = fixture.service.CreateVoicePreset("Fixture created", "Safe description", "Safe reference", "English");
    const auto bank = fixture.service.RenderVoiceBank(fixture.voice.id);
    const auto preview = fixture.service.PreviewVoice(fixture.voice.id, "Safe preview");
    for (const auto& result : {prepared, designed, bank, preview})
    {
        Check(!result.succeeded, "A failed manual operation claimed success.");
        CheckPrivateAbsent(result);
    }
    fixture.backend.invalidPreview = true;
    Check(!fixture.service.PreviewVoice(fixture.voice.id, "Safe preview").succeeded,
        "A missing expected preview WAV reached the playback path.");
    fixture.backend.failManual = false;
    fixture.service.RestoreSynthesisFault("cue.person", fixture.voice.id);
    const auto success = fixture.service.PrepareActiveVoice();
    Check(success.succeeded && success.backend == "low_latency" && success.dtype == "float16",
        "Known safe preparation metadata was not preserved.");
    CheckPrivateAbsent(success);
    Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Degraded, "Manual preparation recovered ordinary health.");
    const auto created = fixture.service.CreateVoicePreset("Fixture created", "Safe description", "Safe reference", "English");
    Check(created.succeeded && created.backend == "low_latency" && created.dtype == "float16" && created.attentionBackend == "auto" &&
              created.inputMode == "complete" && created.sampleRate == 24000 &&
              VoicePresetStore(fixture.directory.root).Find("fixture-created").has_value(),
        "Known safe creation metadata or saved preset was lost.");
    CheckPrivateAbsent(created);
    for (const auto& event : fixture.Events())
    {
        Check((event.detail + event.device).find(Sentinel) == std::string::npos, "A manual Speech event exposed worker payload.");
        Check(event.phase.starts_with("Manual") || event.phase.starts_with("Cue") || event.phase == "SynthesisHealth",
            "Manual diagnostics used an ordinary speech phase.");
        Check(event.utteranceId == 0, "A manual operation or cue status borrowed the active reply turn.");
    }
    VoicePresetStore store(fixture.directory.root);
    const auto partial = SpeechService::FinishVoicePreset(
        store, fixture.voice, [](const auto&, const auto&, const auto&) -> VoiceOperationResult { throw std::runtime_error(Sentinel); });
    Check(partial.succeeded && partial.message == "The voice was created, but its nonverbal clips could not be rendered.",
        "Throwing nonverbal preparation lost the saved voice or exposed raw exception text.");
    fixture.voice.id = "../unsafe";
    CheckPrivateAbsent(SpeechService::FinishVoicePreset(store, fixture.voice, {}));
}

void TestBankKeysAndAtomicPublication()
{
    Fixture fixture;
    auto bank = fixture.Bank();
    Check(bank.Key().size() == 64 && bank.Directory().string().find("cue.person") == std::string::npos,
        "Cue path contained a profile identity instead of a digest.");
    auto changed = fixture.voice;
    changed.language = "Japanese";
    Check(SystemCueBank::ForVoice(fixture.directory.root, "cue.person", changed)->Key() != bank.Key(), "Language did not change cue key.");
    Check(SystemCueBank::ForVoice(fixture.directory.root, "another.person", fixture.voice)->Key() != bank.Key(),
        "Profile did not change cue key.");
    Check(fixture.Bank(2).Key() != bank.Key(), "Phrase version did not change cue key.");
    changed.referenceText = "Another reference.";
    Check(SystemCueBank::ForVoice(fixture.directory.root, "cue.person", changed)->Key() != bank.Key(),
        "Voice fields did not change cue key.");
    const auto originalTime = std::filesystem::last_write_time(fixture.voice.referenceAudioPath);
    {
        std::fstream file(fixture.voice.referenceAudioPath, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(44);
        file.put('Z');
    }
    std::filesystem::last_write_time(fixture.voice.referenceAudioPath, originalTime);
    Check(fixture.Bank().Key() != bank.Key(), "Same-size/time reference content did not change cue key.");
    const auto good = bank.ScratchPath(SystemCueKind::Error);
    revia::tests::WriteMinimalWav(good);
    Check(bank.Publish(SystemCueKind::Error, good), "Validated cue did not publish.");
    const auto final = bank.Clip(SystemCueKind::Error);
    const auto invalid = bank.ScratchPath(SystemCueKind::Error);
    revia::tests::WriteMinimalWav(invalid, 0);
    Check(!bank.Publish(SystemCueKind::Error, invalid) && IsPlayableWavFile(final), "Invalid replace destroyed valid cached cue.");
    Check(!bank.Publish(SystemCueKind::Warning, final), "A published other-kind file was accepted as scratch.");
    const auto other = bank.ScratchPath(SystemCueKind::Warning);
    revia::tests::WriteMinimalWav(other);
    Check(!bank.Publish(SystemCueKind::Error, other), "A different-kind scratch replaced the requested clip.");
    const auto outside = fixture.directory.root / "outside.pending.wav";
    revia::tests::WriteMinimalWav(outside);
    Check(!bank.Publish(SystemCueKind::Error, outside), "An outside artifact entered cue cache.");
    Check(bank.MissingKinds().size() == 6, "Scratch files were advertised as ready clips.");
    const auto replacement = bank.ScratchPath(SystemCueKind::Error);
    revia::tests::WriteMinimalWav(replacement, 8);
    Check(bank.Publish(SystemCueKind::Error, replacement) && std::filesystem::file_size(final) == 52,
        "Atomic replacement did not install a validated replacement over the old final clip.");
    std::filesystem::resize_file(fixture.voice.referenceAudioPath, 33U * 1024U * 1024U);
    Check(
        !SystemCueBank::ForVoice(fixture.directory.root, "cue.person", fixture.voice), "Unbounded reference contents entered cue hashing.");
}

void TestFirstUsePartialAndNoRetry()
{
    Fixture fixture;
    fixture.backend.failCueAt = 3;
    fixture.Fresh();
    auto generator = SpeechServiceTestAccess::StartGenerationWithoutPlayback(fixture.service);
    OnExit cleanup{[&]
        {
            fixture.backend.Release();
            generator.request_stop();
            if (generator.joinable())
                generator.join();
        }};
    Check(WaitUntil([&] { return fixture.service.SystemCueStatusSnapshot().phase == SystemCuePhase::PreparationFailed; }),
        "Actual idle generator did not reach controlled partial failure.");
    Check(fixture.backend.cueRequests == 3 && fixture.Bank().MissingKinds().size() == 5,
        "Partial warming discarded valid clips or continued after first failure.");
    generator.request_stop();
    if (generator.joinable())
        generator.join();
    fixture.Fresh();
    generator = SpeechServiceTestAccess::StartGenerationWithoutPlayback(fixture.service);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Check(fixture.backend.cueRequests == 3, "Unchanged scope retried failed cue preparation.");
    Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Available, "Cue failure degraded ordinary synthesis health.");
    const auto texts = fixture.backend.CueTexts();
    Check(texts == std::vector<std::string>{"Filter.", "Warning.", "Error."}, "Cue transport received unapproved phrases.");
    generator.request_stop();
    if (generator.joinable())
        generator.join();
    fixture.backend.failCueAt = 0;
    SpeechService resumed;
    SpeechServiceTestAccess::ConfigureSynthesisWithoutWorkers(resumed, fixture.settings, fixture.voice, {});
    resumed.SetActiveProfile("cue.person");
    resumed.UseVoice(fixture.voice);
    resumed.Speak("Fresh session harmless reply.", {}, 74);
    SpeechServiceTestAccess::TakeNext(resumed)();
    SpeechServiceTestAccess::ConsumePreparedWithoutPlayback(resumed);
    auto resumedGenerator = SpeechServiceTestAccess::StartGenerationWithoutPlayback(resumed);
    OnExit resumedCleanup{[&]
        {
            fixture.backend.Release();
            resumedGenerator.request_stop();
            if (resumedGenerator.joinable())
                resumedGenerator.join();
        }};
    Check(WaitUntil([&] { return resumed.SystemCueStatusSnapshot().readyClips == 7; }) && fixture.backend.cueRequests == 8,
        "A new session did not preserve valid partial clips and generate only five missing clips.");
}

void TestCompleteGenerationAndCachedPlayback()
{
    Fixture fixture;
    fixture.Fresh();
    auto generator = SpeechServiceTestAccess::StartGenerationWithoutPlayback(fixture.service);
    OnExit cleanup{[&]
        {
            fixture.backend.Release();
            generator.request_stop();
            if (generator.joinable())
                generator.join();
        }};
    Check(WaitUntil([&] { return fixture.service.SystemCueStatusSnapshot().readyClips == 7; }), "Seven approved clips were not cached.");
    generator.request_stop();
    if (generator.joinable())
        generator.join();
    fixture.service.UseVoice(fixture.voice);
    fixture.service.RestoreSynthesisFault("cue.person", fixture.voice.id);
    SpeechServiceTestAccess::SetPlayingTurn(fixture.service, 999);
    const auto requests = fixture.backend.cueRequests.load();
    Check(fixture.service.QueueSystemCue(SystemCueKind::VoiceFailed), "Validated cached cue did not queue.");
    SpeechServiceTestAccess::TakeNext(fixture.service)();
    auto play = SpeechServiceTestAccess::TakeCuePlayback(fixture.service);
    int calls = 0;
    play(
        [&](const auto& path)
        {
            ++calls;
            return IsPlayableWavFile(path);
        });
    Check(calls == 1 && fixture.backend.cueRequests == requests, "Cached playback requested synthesis or bypassed the player boundary.");
    Check(!fixture.service.IsAudioPlaying(), "Cue completion callback left the owned audio channel active.");
    Check(fixture.service.SystemCueStatusSnapshot().phase == SystemCuePhase::Played, "Observed fake playback did not finish cue status.");
    Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Degraded, "Cached cue recovered ordinary health.");
    Check(IsPlayableWavFile(fixture.Bank().Clip(SystemCueKind::VoiceFailed)), "Playback consumed persistent cue.");
    bool played = false;
    for (const auto& event : fixture.Events())
        if (event.phase.starts_with("Cue"))
        {
            Check(event.utteranceId == 0 && !event.synthesis, "Cue borrowed active reply correlation or synthesis observation.");
            if (event.phase == "CuePlayed")
                played = true;
        }
    Check(played, "Cached playback did not publish its own terminal phase.");
}

void TestCachedSelectionAndCancellation()
{
    Fixture fixture;
    fixture.FillCache();
    const auto path = fixture.Bank().Clip(SystemCueKind::Error);
    Check(fixture.service.SystemCueStatusSnapshot().readyClips == 7 && fixture.backend.cueRequests == 0 &&
              fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Unverified,
        "Selection failed to load existing cache without synthesis or claimed ordinary availability.");
    Check(fixture.service.QueueSystemCue(SystemCueKind::Error), "Existing selected cached cue was not queued.");
    fixture.service.StopSpeaking();
    Check(fixture.service.SystemCueStatusSnapshot().phase == SystemCuePhase::Cancelled && IsPlayableWavFile(path),
        "Stop left a queued cue active or destroyed its persistent asset.");
    fixture.service.SetEnabled(false);
    Check(!fixture.service.QueueSystemCue(SystemCueKind::Error), "Muted output accepted a system cue.");
    fixture.service.SetEnabled(true);
    Check(fixture.service.QueueSystemCue(SystemCueKind::Error), "Reenabled cached cue did not queue.");
    auto complete = SpeechServiceTestAccess::TakeNext(fixture.service);
    fixture.service.UseVoice(fixture.voice);
    complete();
    Check(!fixture.service.HasPendingSpeech(), "Old selection cue was published after epoch replacement.");
    Check(fixture.backend.cueRequests == 0, "Cached queue or cancellation called transport.");
    fixture.service.SetEnabled(false);
    fixture.service.SetActiveProfile("muted.person");
    const auto mutedBank = SystemCueBank::ForVoice(fixture.directory.root, "muted.person", fixture.voice);
    Check(mutedBank.has_value(), "Muted fixture scope could not be keyed.");
    const auto scratch = mutedBank->ScratchPath(SystemCueKind::Warning);
    revia::tests::WriteMinimalWav(scratch);
    Check(mutedBank->Publish(SystemCueKind::Warning, scratch), "Muted fixture cached clip did not publish.");
    fixture.service.UseVoice(fixture.voice);
    Check(fixture.service.SystemCueStatusSnapshot().readyClips == 1 && !fixture.service.QueueSystemCue(SystemCueKind::Warning) &&
              fixture.backend.cueRequests == 0,
        "Muted selection hid cached availability or accepted output.");
}

void TestQueuePriorityAndPlaybackGap()
{
    Fixture fixture;
    fixture.backend.blockCue = true;
    fixture.Fresh();
    fixture.service.Speak("Priority ordinary reply.", {}, 74);
    auto generator = SpeechServiceTestAccess::StartGenerationWithoutPlayback(fixture.service);
    OnExit cleanup{[&]
        {
            fixture.backend.Release();
            generator.request_stop();
            if (generator.joinable())
                generator.join();
        }};
    Check(WaitUntil(
              [&]
              {
                  const auto events = fixture.Events();
                  return std::count_if(
                             events.begin(), events.end(), [](const auto& event) { return event.phase == "SynthesisAvailable"; }) >= 2;
              }),
        "Queued ordinary reply did not finish first.");
    Check(fixture.backend.cueRequests == 0, "Cue warming overtook ordinary queued/prepared speech.");
    SpeechServiceTestAccess::ConsumePreparedWithoutPlayback(fixture.service);
    Check(WaitUntil([&] { return fixture.backend.cueGate.Entered(); }), "Idle generator did not request its first cue.");
    fixture.service.Speak("Next ordinary reply.", {}, 75);
    fixture.backend.cueGate.Release();
    Check(WaitUntil([&] { return fixture.backend.ordinaryRequests == 3; }), "Ordinary reply was blocked behind the rest of cue sweep.");
    const auto order = fixture.backend.Order();
    const auto firstCue = std::find(order.begin(), order.end(), "cue:Filter.");
    const auto secondOrdinary = std::find(order.begin(), order.end(), "ordinary:Priority ordinary reply.");
    const auto thirdOrdinary = std::find(order.begin(), order.end(), "ordinary:Next ordinary reply.");
    Check(secondOrdinary < firstCue && firstCue < thirdOrdinary && fixture.backend.cueRequests == 1,
        "Existing generator did not prioritize ordinary work between cue clips.");
}

void TestPreparingCallbackMuteAndInvalidOutput()
{
    SpeechService* owner = nullptr;
    Fixture muted(
        [&](const auto& event)
        {
            if (owner && event.phase == "CuePreparing")
                owner->SetEnabled(false);
        });
    owner = &muted.service;
    muted.Fresh();
    auto generator = SpeechServiceTestAccess::StartGenerationWithoutPlayback(muted.service);
    OnExit cleanup{[&]
        {
            muted.backend.Release();
            generator.request_stop();
            if (generator.joinable())
                generator.join();
        }};
    Check(WaitUntil([&] { return !muted.service.IsEnabled(); }), "CuePreparing callback did not run.");
    Check(muted.backend.cueRequests == 0, "Callback mute was not rechecked before cue transport.");
    Fixture invalid;
    invalid.backend.invalidCue = true;
    invalid.Fresh();
    auto invalidGenerator = SpeechServiceTestAccess::StartGenerationWithoutPlayback(invalid.service);
    OnExit invalidCleanup{[&]
        {
            invalid.backend.Release();
            invalidGenerator.request_stop();
            if (invalidGenerator.joinable())
                invalidGenerator.join();
        }};
    Check(WaitUntil([&] { return invalid.service.SystemCueStatusSnapshot().phase == SystemCuePhase::PreparationFailed; }),
        "Empty-data WAV was reported as a cached cue.");
    Check(
        invalid.Bank().MissingKinds().size() == 7 && invalid.backend.cueRequests == 1, "Invalid partial output was advertised or retried.");
}

void TestInFlightReferenceAndSelectionInvalidation()
{
    for (const bool rewrite : {false, true})
    {
        Fixture fixture;
        fixture.backend.blockCue = true;
        fixture.Fresh();
        const auto bank = fixture.Bank();
        auto generator = SpeechServiceTestAccess::StartGenerationWithoutPlayback(fixture.service);
        OnExit cleanup{[&]
            {
                fixture.backend.Release();
                generator.request_stop();
                if (generator.joinable())
                    generator.join();
            }};
        Check(WaitUntil([&] { return fixture.backend.cueGate.Entered(); }), "Controlled cue request did not enter.");
        if (rewrite)
        {
            std::fstream file(fixture.voice.referenceAudioPath, std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(44);
            file.put('R');
        }
        else
        {
            fixture.service.SetActiveProfile("another.person");
            fixture.service.UseVoice(fixture.voice);
            fixture.service.SetActiveProfile("cue.person");
            fixture.service.UseVoice(fixture.voice);
        }
        fixture.backend.cueGate.Release();
        generator.request_stop();
        if (generator.joinable())
            generator.join();
        Check(bank.MissingKinds().size() == 7 && fixture.service.SystemCueStatusSnapshot().readyClips == 0,
            "Stale reference or old A epoch published a cue.");
        Check(!fixture.service.QueueSystemCue(SystemCueKind::Filter), "Invalidated cached scope was offered for playback.");
        Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Available, "Cue invalidation changed ordinary health.");
    }
}

void TestActiveVoiceReplacementInvalidatesRevision()
{
    for (const bool degraded : {false, true})
    {
        Fixture fixture;
        fixture.backend.blockCue = true;
        fixture.backend.failManual = false;
        fixture.backend.designSamples = 8;
        fixture.Fresh();
        const auto oldBank = fixture.Bank();
        const auto oldSelection = fixture.service.SynthesisHealthSnapshot().selection;
        auto generator = SpeechServiceTestAccess::StartGenerationWithoutPlayback(fixture.service);
        OnExit cleanup{[&]
            {
                fixture.backend.Release();
                generator.request_stop();
                if (generator.joinable())
                    generator.join();
            }};
        Check(WaitUntil([&] { return fixture.backend.cueGate.Entered(); }), "Replacement fixture did not enter old cue transport.");
        if (degraded)
            fixture.service.RestoreSynthesisFault("cue.person", fixture.voice.id);
        const auto result =
            fixture.service.CreateVoicePreset("Fixture voice", "Replacement description", "Replacement reference", "French");
        const auto saved = VoicePresetStore(fixture.directory.root).Find(fixture.voice.id);
        const auto health = fixture.service.SynthesisHealthSnapshot();
        Check(result.succeeded && saved && saved->language == "French" && health.selection.epoch > oldSelection.epoch,
            "Same-slug voice creation did not install a new selected revision.");
        Check(health.state == (degraded ? SynthesisHealth::Degraded : SynthesisHealth::Unverified),
            "Voice replacement retained old verification or erased a degraded fault.");
        Check(fixture.service.SystemCueStatusSnapshot().readyClips == 0 && !fixture.service.QueueSystemCue(SystemCueKind::Filter),
            "Voice replacement retained old cached cues.");
        fixture.backend.cueGate.Release();
        generator.request_stop();
        if (generator.joinable())
            generator.join();
        Check(oldBank.MissingKinds().size() == 7, "Old in-flight cue was published after voice replacement.");
        const auto newBank = SystemCueBank::ForVoice(fixture.directory.root, "cue.person", *saved);
        Check(newBank && newBank->Key() != oldBank.Key(), "Replacement contents and saved fields reused old cue revision.");
        fixture.backend.blockCue = false;
        fixture.Fresh();
        auto resumed = SpeechServiceTestAccess::StartGenerationWithoutPlayback(fixture.service);
        OnExit resumedCleanup{[&]
            {
                fixture.backend.Release();
                resumed.request_stop();
                if (resumed.joinable())
                    resumed.join();
            }};
        Check(WaitUntil([&] { return fixture.service.SystemCueStatusSnapshot().readyClips == 7; }),
            "Fresh ordinary verification did not warm the newly selected revision.");
        Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Available && newBank->MissingKinds().empty(),
            "New revision recovery or cache publication did not use saved voice fields.");
    }
    Fixture cached;
    cached.FillCache();
    Check(cached.service.QueueSystemCue(SystemCueKind::Error), "Cached revision fixture did not queue.");
    auto completeOld = SpeechServiceTestAccess::TakeNext(cached.service);
    cached.backend.failManual = false;
    cached.backend.designSamples = 8;
    Check(cached.service.CreateVoicePreset("Fixture voice", "Replacement description", "Replacement reference", "French").succeeded,
        "Cached revision replacement failed.");
    completeOld();
    Check(!cached.service.HasPendingSpeech(), "Old cached cue survived same-slug revision replacement.");
}

void TestInFlightStopAndShutdown()
{
    for (const bool shutdown : {false, true})
    {
        Fixture fixture;
        fixture.backend.blockCue = true;
        fixture.Fresh();
        auto generator = SpeechServiceTestAccess::StartGenerationWithoutPlayback(fixture.service);
        OnExit cleanup{[&]
            {
                fixture.backend.Release();
                generator.request_stop();
                if (generator.joinable())
                    generator.join();
            }};
        Check(WaitUntil([&] { return fixture.backend.cueGate.Entered(); }), "Cancellation fixture did not start cue transport.");
        if (shutdown)
            fixture.service.RequestVoiceShutdown();
        else
            fixture.service.StopSpeaking();
        fixture.backend.cueGate.Release();
        generator.request_stop();
        if (generator.joinable())
            generator.join();
        Check(fixture.Bank().MissingKinds().size() == 7 && fixture.backend.cueRequests == 1,
            "Cancelled late completion published or retried a cue.");
        Check(!fixture.service.QueueSystemCue(SystemCueKind::Filter), "Shutdown/cancelled cache accepted missing clip.");
        Check(fixture.service.SynthesisHealthSnapshot().state == SynthesisHealth::Available, "Cue cancellation changed ordinary health.");
    }
}

void TestPlaybackBoundaryAndCurrentStatus()
{
    Fixture fixture;
    fixture.FillCache();
    Check(fixture.service.QueueSystemCue(SystemCueKind::Error), "Controlled cached cue did not queue.");
    SpeechServiceTestAccess::TakeNext(fixture.service)();
    auto play = SpeechServiceTestAccess::TakeCuePlayback(fixture.service);
    const auto clip = fixture.Bank().Clip(SystemCueKind::Error);
    revia::tests::WriteMinimalWav(clip, 0);
    int calls = 0;
    play(
        [&](const auto&)
        {
            ++calls;
            return true;
        });
    Check(calls == 0 && fixture.service.SystemCueStatusSnapshot().phase == SystemCuePhase::Unavailable,
        "Corrupted final artifact reached the player boundary.");
    fixture.FillCache();
    Check(fixture.service.QueueSystemCue(SystemCueKind::Warning), "Failure fixture cue did not queue.");
    SpeechServiceTestAccess::TakeNext(fixture.service)();
    SpeechServiceTestAccess::TakeCuePlayback(fixture.service)([](const auto&) { return false; });
    Check(fixture.service.SystemCueStatusSnapshot().phase == SystemCuePhase::PlaybackFailed, "Fake playback failure was not observed.");
    Gate callback;
    SpeechService* owner = nullptr;
    std::atomic<bool> claimObserved = false;
    std::atomic<bool> clearObserved = false;
    Fixture playing(
        [&](const auto& event)
        {
            if (owner && event.phase == "CuePlaying")
            {
                claimObserved = owner->IsAudioPlaying();
                callback.Block();
            }
            if (owner && (event.phase == "CuePlayed" || event.phase == "CueCancelled"))
                clearObserved = !owner->IsAudioPlaying();
        });
    owner = &playing.service;
    playing.FillCache();
    Check(playing.service.QueueSystemCue(SystemCueKind::Error), "Playing fixture cue did not queue.");
    SpeechServiceTestAccess::TakeNext(playing.service)();
    auto playingWork = SpeechServiceTestAccess::TakeCuePlayback(playing.service);
    std::jthread player([&] { playingWork([](const auto&) { return true; }); });
    OnExit cleanup{[&]
        {
            callback.Release();
            if (player.joinable())
                player.join();
        }};
    Check(WaitUntil([&] { return callback.Entered(); }), "Actual shared playback path did not publish CuePlaying.");
    playing.service.SetEnabled(true);
    Check(playing.service.QueueSystemCue(SystemCueKind::Warning) &&
              playing.service.SystemCueStatusSnapshot().phase == SystemCuePhase::Playing,
        "Cache refresh/queue obscured active cue playback.");
    playing.service.SetActiveProfile("another.person");
    playing.service.UseVoice(playing.voice);
    playing.service.SetActiveProfile("cue.person");
    playing.service.UseVoice(playing.voice);
    const auto before = playing.service.SystemCueStatusSnapshot();
    callback.Release();
    if (player.joinable())
        player.join();
    Check(playing.service.SystemCueStatusSnapshot().phase == before.phase && before.readyClips == 7,
        "Old cancelled playback relabeled current A selection.");
    Check(claimObserved && clearObserved, "Cue activity callbacks did not observe the owned channel before/after playback.");
}

bool SameEstimates(const std::vector<VoiceWorkerState>& left, const std::vector<VoiceWorkerState>& right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (left[i].busy != right[i].busy || left[i].fixedOverheadMilliseconds != right[i].fixedOverheadMilliseconds ||
            left[i].millisecondsPerCharacter != right[i].millisecondsPerCharacter || left[i].completed != right[i].completed)
            return false;
    return true;
}

void TestCuePoolAndClientAreNonblocking()
{
    Fixture fixture;
    auto& pool = SpeechServiceTestAccess::VoicePool(fixture.service);
    const auto before = SystemCueTestAccess::WorkerEstimates(pool);
    const auto scratch = fixture.Bank().ScratchPath(SystemCueKind::Error);
    Check(!pool.TrySynthesizeSystemCue(Sentinel, fixture.voice, scratch.string()).succeeded && fixture.backend.cueRequests == 0,
        "Unapproved phrase reached the controlled cue transport.");
    Check(pool.TrySynthesizeSystemCue("Error.", fixture.voice, scratch.string()).succeeded, "Direct controlled cue failed.");
    fixture.backend.failCueAt = 2;
    Check(!pool.TrySynthesizeSystemCue("Error.", fixture.voice, scratch.string()).succeeded, "Controlled cue failure succeeded.");
    Check(SameEstimates(before, SystemCueTestAccess::WorkerEstimates(pool)),
        "Cue work trained ordinary routing estimates/completed samples.");
    for (const bool client : {false, true})
    {
        Gate gate;
        std::jthread holder(
            [&]
            {
                if (client)
                    SystemCueTestAccess::WithClientLock(pool, [&] { gate.Block(); });
                else
                    SystemCueTestAccess::WithPoolLock(pool, [&] { gate.Block(); });
            });
        OnExit cleanup{[&]
            {
                gate.Release();
                if (holder.joinable())
                    holder.join();
            }};
        Check(WaitUntil([&] { return gate.Entered(); }), "Mutex fixture did not enter.");
        const auto start = std::chrono::steady_clock::now();
        const auto result = pool.TrySynthesizeSystemCue("Error.", fixture.voice, scratch.string());
        const auto elapsed = std::chrono::steady_clock::now() - start;
        gate.Release();
        if (holder.joinable())
            holder.join();
        Check(!result.succeeded && elapsed < std::chrono::milliseconds(300), "Cue waited for occupied pool/client.");
    }
    fixture.backend.blockOrdinary = true;
    std::jthread ordinary([&] { fixture.service.RenderAdapterSpeech("A safe public reply."); });
    OnExit cleanup{[&]
        {
            fixture.backend.Release();
            if (ordinary.joinable())
                ordinary.join();
        }};
    Check(WaitUntil([&] { return fixture.backend.ordinaryGate.Entered(); }), "Busy worker fixture did not enter real ordinary HTTP.");
    const auto start = std::chrono::steady_clock::now();
    Check(!pool.TrySynthesizeSystemCue("Error.", fixture.voice, scratch.string()).succeeded &&
              std::chrono::steady_clock::now() - start < std::chrono::milliseconds(300),
        "Cue waited behind a busy worker.");
}

class DripPeer
{
  public:
    explicit DripPeer(const bool header)
    {
        listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Check(listener != INVALID_SOCKET && ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                  ::listen(listener, 1) == 0,
            "Could not bind controlled drip fixture.");
        int bytes = sizeof(address);
        Check(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &bytes) == 0, "Could not read drip fixture port.");
        port = ntohs(address.sin_port);
        thread = std::jthread(
            [&, header](const std::stop_token token)
            {
                const auto connection = accept(listener, nullptr, nullptr);
                if (connection == INVALID_SOCKET)
                    return;
                std::array<char, 8192> request{};
                recv(connection, request.data(), static_cast<int>(request.size()), 0);
                const std::string headers =
                    "HTTP/1.1 200 OK\r\nContent-Length: 4096\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n";
                if (!header)
                    send(connection, headers.data(), static_cast<int>(headers.size()), 0);
                const auto payload = header ? headers : std::string(100, 'x');
                for (const char byte : payload)
                {
                    if (token.stop_requested() || send(connection, &byte, 1, 0) <= 0)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                closesocket(connection);
            });
    }
    ~DripPeer()
    {
        thread.request_stop();
        closesocket(listener);
        thread.join();
    }
    int port = 0;

  private:
    SOCKET listener = INVALID_SOCKET;
    std::jthread thread;
};

void TestCueTotalDeadlineAndUnavailable()
{
    Fixture fixture;
    const auto scratch = fixture.Bank().ScratchPath(SystemCueKind::Error);
    fixture.backend.blockCue = true;
    auto& stalledPool = SpeechServiceTestAccess::VoicePool(fixture.service);
    const auto stalledStart = std::chrono::steady_clock::now();
    const auto stalledResult = stalledPool.TrySynthesizeSystemCue("Error.", fixture.voice, scratch.string());
    const auto stalledElapsed = std::chrono::steady_clock::now() - stalledStart;
    fixture.backend.Release();
    Check(!stalledResult.succeeded && stalledElapsed >= std::chrono::seconds(4) && stalledElapsed < std::chrono::milliseconds(5600),
        "Silent HTTP stall extended the cue deadline.");
    for (const bool header : {false, true})
    {
        DripPeer peer(header);
        auto settings = fixture.settings;
        settings.qwenPort = peer.port;
        settings.pythonExecutable = "not-a-fixture-executable";
        QwenTtsPool pool;
        pool.Configure(settings);
        const auto start = std::chrono::steady_clock::now();
        const auto result = pool.TrySynthesizeSystemCue("Error.", fixture.voice, scratch.string());
        const auto elapsed = std::chrono::steady_clock::now() - start;
        Check(!result.succeeded && elapsed >= std::chrono::seconds(4) && elapsed < std::chrono::milliseconds(5600),
            "Slow header/body drip extended the total cue request deadline.");
    }
    auto settings = fixture.settings;
    settings.qwenHost = "unresolved-cue-fixture.invalid";
    settings.pythonExecutable = "not-a-fixture-executable";
    QwenTtsPool pool;
    pool.Configure(settings);
    const auto start = std::chrono::steady_clock::now();
    Check(!pool.TrySynthesizeSystemCue("Error.", fixture.voice, scratch.string()).succeeded &&
              std::chrono::steady_clock::now() - start < std::chrono::milliseconds(300),
        "Cue attempted DNS/worker startup for unsupported host.");
    Check(!pool.TrySynthesizeSystemCue(Sentinel, fixture.voice, scratch.string()).succeeded, "Unapproved phrase entered cue transport.");
}

void TestAttemptBudgetAndCachedSuccessDoesNotWarm()
{
    Fixture fixture;
    fixture.backend.cached = true;
    fixture.Fresh();
    auto generator = SpeechServiceTestAccess::StartGenerationWithoutPlayback(fixture.service);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    generator.request_stop();
    if (generator.joinable())
        generator.join();
    Check(fixture.backend.cueRequests == 0, "Cached ordinary audio scheduled cue preparation.");
    fixture.backend.cached = false;
    fixture.backend.failCueAt = 1;
    for (int scope = 0; scope < 65; ++scope)
    {
        fixture.service.SetActiveProfile("budget." + std::to_string(scope));
        fixture.service.UseVoice(fixture.voice);
        fixture.Fresh();
        generator = SpeechServiceTestAccess::StartGenerationWithoutPlayback(fixture.service);
        OnExit cleanup{[&]
            {
                fixture.backend.Release();
                generator.request_stop();
                if (generator.joinable())
                    generator.join();
            }};
        if (scope < 64)
            Check(WaitUntil([&] { return fixture.service.SystemCueStatusSnapshot().phase == SystemCuePhase::PreparationFailed; }),
                "Cue scope did not finish its bounded attempt.");
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        generator.request_stop();
        if (generator.joinable())
            generator.join();
    }
    Check(fixture.backend.cueRequests == 64, "Cue scope budget grew or evicted scopes into repeat attempts.");
}
}

void RunSystemCueTests()
{
    TestCuePairReservation();
    TestManualDiagnostics();
    TestManualOperationsRetainOrigin();
    TestActiveVoiceReplacementInvalidatesRevision();
    TestBankKeysAndAtomicPublication();
    TestFirstUsePartialAndNoRetry();
    TestCompleteGenerationAndCachedPlayback();
    TestCachedSelectionAndCancellation();
    TestQueuePriorityAndPlaybackGap();
    TestPreparingCallbackMuteAndInvalidOutput();
    TestInFlightReferenceAndSelectionInvalidation();
    TestInFlightStopAndShutdown();
    TestPlaybackBoundaryAndCurrentStatus();
    TestCuePoolAndClientAreNonblocking();
    TestCueTotalDeadlineAndUnavailable();
    TestAttemptBudgetAndCachedSuccessDoesNotWarm();
    std::cout << "System cue and manual diagnostics tests passed (16 fixtures).\n";
}
