#include "testSupport.h"
#include "speechServiceTestAccess.h"

#include "Library/structLibrary.h"
#include "Speech/pcmStream.h"
#include "Speech/speechService.h"
#include "Speech/voiceTypes.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The voice she has when hers is down.
//
// A phrase every Qwen worker fails is handed to Kokoro, on its own port and contract,
// before Windows SAPI ever hears of it; the result says which voice spoke; a Kokoro
// that also fails leaves a failure naming both; and with the fallback off, nothing is
// sent anywhere.
namespace
{
using namespace std::chrono_literals;
using revia::speech::SpeechEvent;
using revia::speech::SpeechService;
using revia::speech::SpeechServiceTestAccess;
using revia::speech::VoiceOperationResult;
using revia::speech::VoicePreset;
using revia::speech::WrapPcmAsWav;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;

// Kokoro as the client sees it: a health answer and a WAV with the worker's headers,
// or a refusal.
class KokoroFixture
{
public:
    explicit KokoroFixture(const bool refuses)
    {
        server.Get("/health", [](const auto&, auto& response)
        {
            response.set_content(
                R"({"status":"ok","backend":"kokoro","model_kind":"kokoro","device":"cpu"})",
                "application/json");
        });
        server.Post("/v1/audio/pcm", [this, refuses](const auto& request, auto& response)
        {
            {
                std::lock_guard lock(mutex);
                bodies.push_back(request.body);
            }
            if (refuses)
            {
                response.status = 500;
                response.set_content(
                    R"({"succeeded":false,"message":"Kokoro failed: the voice file is missing."})",
                    "application/json");
                return;
            }
            const std::vector<std::uint8_t> wav = WrapPcmAsWav({7, 8, 9}, 24000);
            response.set_header("X-Revia-Sample-Rate", "24000");
            response.set_header("X-Revia-Elapsed-Ms", "12.5");
            response.set_header("X-Revia-Device", "cpu");
            response.set_header("X-Revia-Device-Name", "Kokoro");
            response.set_header("X-Revia-Backend", "kokoro");
            response.set_content(std::string(wav.begin(), wav.end()), "audio/wav");
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind the Kokoro fixture.");
        thread = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        Check(server.is_running(), "The Kokoro fixture did not start.");
    }
    ~KokoroFixture() { server.stop(); thread.join(); }
    std::vector<std::string> Bodies() { std::lock_guard lock(mutex); return bodies; }
    int port = 0;
private:
    httplib::Server server;
    std::mutex mutex;
    std::vector<std::string> bodies;
    std::jthread thread;
};

VoicePreset Preset(const ScopedTestDirectory& directory)
{
    VoicePreset preset;
    preset.id = "fixture";
    preset.name = "Fixture";
    preset.language = "English";
    preset.referenceText = "Reference.";
    preset.referenceAudioPath = (directory.root / "reference.wav").string();
    revia::tests::WriteMinimalWav(directory.root / "reference.wav");
    return preset;
}

// Her own voice with no worker behind it: a closed port and no script to start one
// from, so the Qwen attempt fails at once instead of waiting on a startup that
// cannot happen.
speechSettings Settings(const int kokoroPort, const std::filesystem::path& root)
{
    speechSettings settings;
    settings.bEnabled = true;
    settings.backend = "Qwen";
    settings.voiceDataPath = root.string();
    settings.bQwenDirectPcm = true;
    settings.bQwenStreamFirstPhrase = false;
    settings.bQwenBatchReplyPhrases = false;
    settings.qwenHost = "127.0.0.1";
    settings.qwenPort = 1;
    settings.qwenServiceScript = (root / "no-such-worker.py").string();
    settings.qwenStartupTimeoutSeconds = 1;
    settings.qwenRequestTimeoutSeconds = 5;
    settings.bFallbackVoiceEnabled = true;
    settings.fallbackVoicePort = kokoroPort;
    settings.fallbackVoice = "bf_emma";
    return settings;
}

struct Voiced
{
    std::vector<VoiceOperationResult> results;
    std::vector<SpeechEvent> fallbackEvents;
};

Voiced SpeakOnePhrase(const speechSettings& settings, const ScopedTestDirectory& directory)
{
    SpeechService service;
    SpeechServiceTestAccess::ConfigureWithoutWorkers(service, directory.root);
    SpeechServiceTestAccess::Configuration(service) = settings;
    SpeechServiceTestAccess::VoicePool(service).Configure(settings);
    SpeechServiceTestAccess::ConfigureFallbackVoice(service, settings);
    auto events = std::make_shared<std::vector<SpeechEvent>>();
    auto eventsMutex = std::make_shared<std::mutex>();
    SpeechServiceTestAccess::ObserveEvents(service, [events, eventsMutex](const SpeechEvent& event)
    {
        if (event.phase != "Fallback") return;
        std::lock_guard lock(*eventsMutex);
        events->push_back(event);
    });
    service.UseVoice(Preset(directory));
    service.Speak("Hello there.", {}, 41);
    SpeechServiceTestAccess::TakeNext(service)();
    Voiced voiced;
    voiced.results = SpeechServiceTestAccess::PreparedResults(service);
    {
        std::lock_guard lock(*eventsMutex);
        voiced.fallbackEvents = *events;
    }
    SpeechServiceTestAccess::ObserveEvents(service, {});
    return voiced;
}

void TestAPhraseQwenCannotVoiceIsSpokenByKokoro()
{
    ScopedTestDirectory directory;
    KokoroFixture kokoro(false);
    const Voiced voiced = SpeakOnePhrase(Settings(kokoro.port, directory.root), directory);
    Check(voiced.results.size() == 1, "One phrase did not prepare as one item.");
    const VoiceOperationResult& result = voiced.results.front();
    Check(result.succeeded, "Kokoro's audio was not taken for the phrase: " + result.message);
    Check(result.device == "cpu" && result.deviceName == "Kokoro",
        "The result does not say Kokoro spoke it (" + result.device + " / " + result.deviceName + ").");
    Check(result.audioBytes.size() == 44 + 6 && result.sampleRate == 24000,
        "The fallback audio did not arrive as the WAV Kokoro sent.");
    const auto bodies = kokoro.Bodies();
    Check(bodies.size() == 1 && bodies.front().find("\"text\":\"Hello there.\"") != std::string::npos,
        "Kokoro was not asked for exactly the phrase.");
    Check(voiced.fallbackEvents.size() == 1 &&
        voiced.fallbackEvents.front().detail.find("Kokoro") != std::string::npos &&
        voiced.fallbackEvents.front().device == "CPU / Kokoro" &&
        voiced.fallbackEvents.front().utteranceId == 41,
        "The Activity panel was not told the stand-in voice took the phrase.");
}

void TestAKokoroThatAlsoFailsLeavesAFailureNamingBoth()
{
    ScopedTestDirectory directory;
    KokoroFixture kokoro(true);
    const Voiced voiced = SpeakOnePhrase(Settings(kokoro.port, directory.root), directory);
    Check(voiced.results.size() == 1 && !voiced.results.front().succeeded,
        "A refused fallback was reported as audio.");
    const std::string& message = voiced.results.front().message;
    const auto marker = message.find(" Kokoro: ");
    Check(marker != std::string::npos && marker > 0 && marker + 9 < message.size(),
        "The failure does not name both attempts: " + message);
    Check(kokoro.Bodies().size() == 1, "Kokoro was not asked once.");
}

void TestWithTheFallbackOffNothingIsSentAnywhere()
{
    ScopedTestDirectory directory;
    KokoroFixture kokoro(false);
    speechSettings settings = Settings(kokoro.port, directory.root);
    settings.bFallbackVoiceEnabled = false;
    const Voiced voiced = SpeakOnePhrase(settings, directory);
    Check(voiced.results.size() == 1 && !voiced.results.front().succeeded &&
        voiced.results.front().message.find("Kokoro") == std::string::npos,
        "With the fallback off, the phrase still went looking for Kokoro.");
    Check(kokoro.Bodies().empty() && voiced.fallbackEvents.empty(),
        "With the fallback off, Kokoro was still asked or announced.");
}

void TestTheFallbackWorkerRunsUnderItsOwnSettings()
{
    speechSettings settings;
    settings.pythonExecutable = "ThirdParty/QwenTTS/.venv/Scripts/python.exe";
    settings.qwenServiceScript = "Tools/qwen_tts_service.py";
    settings.qwenPort = 8092;
    settings.qwenDevice = "cuda:0";
    settings.qwenDevices = {"cuda:0", "cuda:1"};
    settings.qwenWorkerArguments = "--cpu-threads 8";
    settings.bQwenLowLatencyPhrase = true;
    settings.fallbackVoiceScript = "Tools/kokoro_tts_service.py";
    settings.fallbackVoicePythonExecutable = "ThirdParty/Kokoro/.venv/Scripts/python.exe";
    settings.fallbackVoicePort = 8097;
    settings.fallbackVoice = "jf_alpha";
    const speechSettings fallback = SpeechServiceTestAccess::FallbackVoiceSettings(settings);
    Check(fallback.qwenServiceScript == "Tools/kokoro_tts_service.py" &&
        fallback.pythonExecutable == "ThirdParty/Kokoro/.venv/Scripts/python.exe" &&
        fallback.qwenPort == 8097 && fallback.qwenHost == settings.qwenHost,
        "The fallback worker is not launched from the Kokoro script, environment and port.");
    Check(fallback.qwenDevice == "cpu" && fallback.qwenDevices == std::vector<std::string>{"cpu"} &&
        !fallback.bQwenLowLatencyPhrase,
        "The fallback worker was not kept off the GPU.");
    Check(fallback.qwenWorkerArguments == "--voice jf_alpha",
        "The Kokoro voice did not reach the worker's command line: " + fallback.qwenWorkerArguments);
    settings.fallbackVoice = "af heart; del *";
    Check(SpeechServiceTestAccess::FallbackVoiceSettings(settings).qwenWorkerArguments == "--voice af_heart",
        "A voice id that is not one was allowed onto the command line.");
}
} // namespace

void RunFallbackVoiceTests()
{
    TestAPhraseQwenCannotVoiceIsSpokenByKokoro();
    TestAKokoroThatAlsoFailsLeavesAFailureNamingBoth();
    TestWithTheFallbackOffNothingIsSentAnywhere();
    TestTheFallbackWorkerRunsUnderItsOwnSettings();
    std::cout << "A phrase her voice cannot speak goes to Kokoro before SAPI, says so, "
                 "and stays home with the fallback off.\n";
}
