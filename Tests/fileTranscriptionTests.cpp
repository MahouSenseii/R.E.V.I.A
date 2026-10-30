#include "Speech/fileTranscription.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
using Json = nlohmann::json;

std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void Write(const std::filesystem::path& path, const std::string& bytes)
{
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
}

#ifdef REVIA_FILE_TRANSCRIPTION_FIXTURE
int main(const int argc, char** argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    const auto argument = [&](const std::string& key)
    {
        const auto found = std::find(args.begin(), args.end(), key);
        if (found == args.end() || found + 1 == args.end())
            throw std::runtime_error("Missing fixture argument: " + key);
        return *(found + 1);
    };
    try
    {
        const auto model = std::filesystem::u8path(argument("-m"));
        const Json spec = Json::parse(Read(model));
        Write(model.string() + ".started", "started");
        Write(model.string() + ".args.json", Json(args).dump());
        const auto mode = spec.value("mode", "json");
        if (mode == "result-file-flood" || mode == "result-directory")
        {
            const auto output = std::filesystem::u8path(argument("-of") + ".json");
            if (mode == "result-file-flood")
                Write(output, std::string(256 * 1024 + 1, 'x'));
            else
                std::filesystem::create_directory(output);
            Write(model.string() + ".result-ready", "ready");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            Write(model.string() + ".completed", "completed");
            return 0;
        }
        if (mode == "slow")
            std::this_thread::sleep_for(std::chrono::seconds(5));
        if (mode == "flood")
            std::cout << std::string(512 * 1024, 'x') << std::flush;
        if (mode != "missing")
        {
            const auto output = std::filesystem::u8path(argument("-of") + ".json");
            const std::string body =
                mode == "invalid" ? "{not json"
                : mode == "invalid-utf8"
                    ? std::string("{\"transcription\":[{\"offsets\":{\"from\":0,\"to\":100},\"text\":\"") + static_cast<char>(255) + "\"}]}"
                    : spec.value("response", Json::object()).dump();
            Write(output, body);
        }
        return spec.value("exitCode", 0);
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what();
        return 2;
    }
}
#else
namespace
{
void Check(const bool value, const std::string& message)
{
    if (!value)
        throw std::runtime_error(message);
}

struct Fixture
{
    std::filesystem::path root;
    std::filesystem::path wave;
    std::filesystem::path model;
    std::filesystem::path vad;
    speechRecognitionSettings settings;

    Fixture()
    {
        root = std::filesystem::temp_directory_path() /
               ("revia-file-transcription-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(root);
        wave = root / "source with spaces.wav";
        model = root / "model with spaces.json";
        vad = root / "vad model with spaces.bin";
        Write(vad, "local VAD fixture");
#ifdef _WIN32
        wchar_t module[32768]{};
        Check(GetModuleFileNameW(nullptr, module, 32768) != 0, "Could not find transcription test executable.");
        settings.executable = (std::filesystem::path(module).parent_path() / "ReviaFileTranscriptionFixture.exe").string();
#else
        settings.executable = (std::filesystem::current_path() / "ReviaFileTranscriptionFixture").string();
#endif
        settings.modelPath = model.string();
        settings.bEnabled = false;
        settings.bUseServer = false;
        settings.bUseGpu = false;
        settings.device = "cpu";
        settings.threads = 2;
        settings.requestTimeoutSeconds = 3;
        SetSpec({});
        SetWave();
    }

    ~Fixture()
    {
        std::error_code error;
        if (root.parent_path() == std::filesystem::temp_directory_path() &&
            root.filename().string().starts_with("revia-file-transcription-tests-"))
            std::filesystem::remove_all(root, error);
    }

    void SetWave(const int rate = 16000, const int channels = 1, const int bits = 16, const std::uint32_t samples = 16000)
    {
        std::string bytes;
        const auto put = [&](const std::uint32_t value, const int count)
        {
            for (int i = 0; i < count; ++i)
                bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
        };
        const std::uint32_t data = samples * channels * bits / 8;
        bytes += "RIFF";
        put(36 + data, 4);
        bytes += "WAVEfmt ";
        put(16, 4);
        put(1, 2);
        put(channels, 2);
        put(rate, 4);
        put(rate * channels * bits / 8, 4);
        put(channels * bits / 8, 2);
        put(bits, 2);
        bytes += "data";
        put(data, 4);
        bytes.append(data, '\0');
        Write(wave, bytes);
    }

    void SetSpec(Json spec)
    {
        if (!spec.contains("response"))
            spec["response"] = {{"transcription", {{{"offsets", {{"from", 100}, {"to", 600}}}, {"text", " Quartz nineteen."}},
                                                      {{"offsets", {{"from", 650}, {"to", 900}}}, {"text", " meadow."}}}}};
        Write(model, spec.dump());
        std::filesystem::remove(model.string() + ".started");
        std::filesystem::remove(model.string() + ".args.json");
        std::filesystem::remove(model.string() + ".result-ready");
        std::filesystem::remove(model.string() + ".completed");
    }

    void NoTemporaryResults() const
    {
        for (const auto& entry : std::filesystem::directory_iterator(root))
            Check(
                !entry.path().filename().string().starts_with(".revia-transcribe-"), "Transcription left owned temporary results behind.");
    }
};

void TestValidFilePreservesInputAndParsesTimestamps()
{
    Fixture fixture;
    const auto before = Read(fixture.wave);
    const auto result = revia::speech::TranscribeWaveFile(fixture.wave, fixture.settings);
    Check(result.ok && !result.cancelled && result.error.empty() && result.segments.size() == 2,
        "Valid timestamped CLI output was not returned as file transcription data: " + result.error);
    Check(result.segments[0].startMs == 100 && result.segments[0].endMs == 600 && result.segments[0].text == "Quartz nineteen." &&
              result.segments[1].startMs == 650 && result.segments[1].endMs == 900 && result.segments[1].text == "meadow.",
        "Transcription changed literal text or file-relative timestamp offsets.");
    Check(Read(fixture.wave) == before, "Transcription deleted or modified the caller's input file.");
    const auto args = Json::parse(Read(fixture.model.string() + ".args.json")).get<std::vector<std::string>>();
    Check(std::find(args.begin(), args.end(), "-oj") != args.end() && std::find(args.begin(), args.end(), "-ng") != args.end(),
        "File transcription lost timestamped JSON output or explicit CPU execution.");
    Check(std::find(args.begin(), args.end(), "--vad") == args.end() && std::find(args.begin(), args.end(), "--vad-model") == args.end(),
        "An unchanged caller silently enabled uploaded-file VAD.");
    fixture.NoTemporaryResults();
}

void TestExplicitVadIsRequiredAndPassedToTheCli()
{
    Fixture fixture;
    const auto input = Read(fixture.wave);
    const auto result = revia::speech::TranscribeWaveFile(fixture.wave, fixture.settings, {}, fixture.vad);
    Check(result.ok && result.segments.size() == 2 && result.segments[0].startMs == 100 && result.segments[1].endMs == 900,
        "Explicit VAD changed or lost file-relative transcription data.");
    const auto args = Json::parse(Read(fixture.model.string() + ".args.json")).get<std::vector<std::string>>();
    Check(std::count(args.begin(), args.end(), "--vad") == 1, "Explicit uploaded-file VAD did not reach the CLI exactly once.");
    const auto value = [&](const std::string& flag)
    {
        const auto found = std::find(args.begin(), args.end(), flag);
        Check(found != args.end() && found + 1 != args.end(), "Missing VAD CLI argument: " + flag);
        return *(found + 1);
    };
    Check(value("--vad-model") == std::filesystem::canonical(fixture.vad).string(), "VAD model path was not passed as one local argument.");
    Check(value("--vad-threshold") == "0.50" && value("--vad-min-speech-duration-ms") == "250" &&
              value("--vad-min-silence-duration-ms") == "100" && value("--vad-speech-pad-ms") == "30" &&
              value("--vad-samples-overlap") == "0.10",
        "The qualified default VAD timing/threshold contract changed.");
    fixture.NoTemporaryResults();

    for (const auto& invalid : {fixture.root / "missing-vad.bin", fixture.root / "vad-directory", fixture.root / "empty-vad.bin"})
    {
        fixture.SetSpec({});
        if (invalid.filename() == "vad-directory")
            std::filesystem::create_directory(invalid);
        else if (invalid.filename() == "empty-vad.bin")
            Write(invalid, "");
        const auto refused = revia::speech::TranscribeWaveFile(fixture.wave, fixture.settings, {}, invalid);
        Check(!refused.ok && !refused.cancelled && refused.segments.empty() && refused.error.find("VAD") != std::string::npos,
            "A missing or invalid explicit VAD silently fell back to unfiltered ASR.");
        Check(!std::filesystem::exists(fixture.model.string() + ".started"), "An unavailable explicit VAD started a child process.");
        fixture.NoTemporaryResults();
    }
    fixture.SetSpec({{"response", {{"transcription", Json::array()}}}});
    const auto noSpeech = revia::speech::TranscribeWaveFile(fixture.wave, fixture.settings, {}, fixture.vad);
    Check(noSpeech.ok && noSpeech.segments.empty(), "A valid empty native VAD result fabricated spoken words or became a provider error.");
    Check(Read(fixture.wave) == input, "Explicit VAD changed or removed the caller's input WAV.");
    fixture.NoTemporaryResults();
}

void TestInputBoundsRefuseBeforeLaunching()
{
    Fixture fixture;
    const auto refused = [&]()
    {
        const auto result = revia::speech::TranscribeWaveFile(fixture.wave, fixture.settings);
        Check(!result.ok && !result.cancelled && !result.error.empty() && result.segments.empty(),
            "Malformed or unbounded WAV was accepted.");
        Check(!std::filesystem::exists(fixture.model.string() + ".started"), "Invalid input reached the CLI before validation.");
        fixture.NoTemporaryResults();
    };
    Write(fixture.wave, "RIFF");
    refused();
    fixture.SetWave(8000);
    refused();
    fixture.SetWave(16000, 2);
    refused();
    fixture.SetWave(16000, 1, 8);
    refused();
    fixture.SetWave(16000, 1, 16, 960001);
    refused();
    Write(fixture.wave, std::string(2 * 1024 * 1024 + 1, 'x'));
    refused();
    fixture.SetWave();
    auto truncated = Read(fixture.wave);
    truncated.resize(100);
    Write(fixture.wave, truncated);
    refused();
}

void TestUntrustedCliOutputIsBounded()
{
    Fixture fixture;
    const std::vector<Json> invalid = {{{"mode", "invalid"}}, {{"mode", "invalid-utf8"}}, {{"mode", "missing"}}, {{"exitCode", 7}},
        {{"mode", "flood"}}, {{"response", {{"transcription", {{{"offsets", {{"from", -1}, {"to", 400}}}, {"text", "bad"}}}}}}},
        {{"response", {{"transcription", {{{"offsets", {{"from", 900}, {"to", 1200}}}, {"text", "bad"}}}}}}},
        {{"response", {{"transcription", {{{"offsets", {{"from", 800}, {"to", 200}}}, {"text", "bad"}}}}}}},
        {{"response", {{"transcription", {{{"offsets", {{"from", 0.5}, {"to", 200}}}, {"text", "bad"}}}}}}},
        {{"response", {{"transcription", {{{"offsets", {{"from", 0}, {"to", 200}}}, {"text", std::string(70000, 'x')}}}}}}}};
    for (const auto& spec : invalid)
    {
        fixture.SetSpec(spec);
        const auto result = revia::speech::TranscribeWaveFile(fixture.wave, fixture.settings);
        Check(!result.ok && !result.error.empty() && result.segments.empty(), "Invalid, failed or excessive CLI output was accepted.");
        Check(std::filesystem::exists(fixture.wave), "Failed CLI work removed the caller's WAV.");
        fixture.NoTemporaryResults();
    }
    fixture.SetSpec({{"response", {{"transcription", {{{"offsets", {{"from", 0}, {"to", 1000}}}, {"text", "[BLANK_AUDIO]"}}}}}}});
    const auto blank = revia::speech::TranscribeWaveFile(fixture.wave, fixture.settings);
    Check(blank.ok && blank.segments.empty(), "Blank-audio marker was exposed as recognized speech or a fabricated success message.");
    fixture.NoTemporaryResults();
}

void TestDeadlineAndCancellationStopOwnedWork()
{
    Fixture fixture;
    fixture.SetSpec({{"mode", "slow"}});
    fixture.settings.requestTimeoutSeconds = 1;
    const auto start = std::chrono::steady_clock::now();
    const auto timed = revia::speech::TranscribeWaveFile(fixture.wave, fixture.settings);
    Check(!timed.ok && !timed.cancelled && !timed.error.empty() && std::chrono::steady_clock::now() - start < std::chrono::seconds(3),
        "File transcription ignored its strict deadline.");
    fixture.NoTemporaryResults();
    fixture.SetSpec({{"mode", "slow"}});
    fixture.settings.requestTimeoutSeconds = 3;
    std::stop_source stop;
    auto pending = std::async(std::launch::async,
        [&]() { return revia::speech::TranscribeWaveFile(fixture.wave, fixture.settings, stop.get_token(), fixture.vad); });
    const auto ready = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!std::filesystem::exists(fixture.model.string() + ".started") && std::chrono::steady_clock::now() < ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(std::filesystem::exists(fixture.model.string() + ".started"), "Cancellation fixture never entered owned CLI work.");
    stop.request_stop();
    const auto cancelled = pending.get();
    Check(cancelled.cancelled && !cancelled.ok && cancelled.segments.empty(), "Cancelled file transcription published a result.");
    fixture.NoTemporaryResults();
    fixture.SetSpec({});
    const auto before = revia::speech::TranscribeWaveFile(fixture.wave, fixture.settings, stop.get_token(), fixture.vad);
    Check(before.cancelled && !std::filesystem::exists(fixture.model.string() + ".started"), "Pre-cancelled transcription started a CLI.");
}

void TestResultFileBoundsStopOwnedWorkBeforeExit()
{
    Fixture fixture;
    const auto input = Read(fixture.wave);
    fixture.settings.requestTimeoutSeconds = 5;
    for (const auto* mode : {"result-file-flood", "result-directory"})
    {
        fixture.SetSpec({{"mode", mode}});
        auto pending = std::async(
            std::launch::async, [&]() { return revia::speech::TranscribeWaveFile(fixture.wave, fixture.settings, {}, fixture.vad); });
        const auto ready = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!std::filesystem::exists(fixture.model.string() + ".result-ready") && std::chrono::steady_clock::now() < ready)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const bool produced = std::filesystem::exists(fixture.model.string() + ".result-ready");
        const bool stoppedEarly = pending.wait_for(std::chrono::milliseconds(750)) == std::future_status::ready;
        const auto result = pending.get();
        fixture.NoTemporaryResults();
        Check(Read(fixture.wave) == input, "Result-file rejection changed or removed the caller's WAV.");
        Check(produced, "Result-file fixture did not produce its invalid result while running.");
        Check(stoppedEarly, "Transcription waited for CLI exit instead of enforcing its result-file bound while running.");
        Check(!std::filesystem::exists(fixture.model.string() + ".completed"), "The invalid-result CLI was not terminated early.");
        Check(!result.ok && !result.cancelled && result.segments.empty() && result.error.find("result file") != std::string::npos,
            "Invalid result-file work did not report a bounded file error.");
    }
}

void TestInstalledCliResolutionSurvivesAnIsolatedRuntime()
{
    Fixture fixture;
    const auto before = std::filesystem::current_path();
    std::filesystem::create_directory(fixture.root / "Config");
    fixture.settings.executable = "ReviaFileTranscriptionFixture.exe";
    std::filesystem::current_path(fixture.root);
    revia::speech::FileTranscriptionResult result;
    try
    {
        result = revia::speech::TranscribeWaveFile(fixture.wave, fixture.settings);
        std::filesystem::current_path(before);
    }
    catch (...)
    {
        std::filesystem::current_path(before);
        throw;
    }
    Check(result.ok && result.segments.size() == 2,
        "An isolated runtime Config hid the CLI installed beside the executable: " + result.error);
    fixture.NoTemporaryResults();
}
}

void RunFileTranscriptionTests()
{
    TestValidFilePreservesInputAndParsesTimestamps();
    TestExplicitVadIsRequiredAndPassedToTheCli();
    TestInputBoundsRefuseBeforeLaunching();
    TestUntrustedCliOutputIsBounded();
    TestResultFileBoundsStopOwnedWorkBeforeExit();
    TestDeadlineAndCancellationStopOwnedWork();
    TestInstalledCliResolutionSurvivesAnIsolatedRuntime();
    std::cout << "File transcription input, timestamp, output, cancellation and cleanup checks passed.\n";
}

#ifdef REVIA_FILE_TRANSCRIPTION_TEST_MAIN
int main()
{
    try
    {
        RunFileTranscriptionTests();
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
#endif
#endif
