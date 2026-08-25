#include "Speech/speechRecognitionService.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <deque>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

namespace revia::speech
{

namespace
{
    double ElapsedMilliseconds(const std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }

    void WriteLittleEndian(std::ofstream& stream, const std::uint32_t value, const int bytes)
    {
        for (int index = 0; index < bytes; ++index)
        {
            stream.put(static_cast<char>((value >> (index * 8)) & 0xff));
        }
    }

    bool WriteWaveFile(
        const std::filesystem::path& path,
        const std::vector<std::uint8_t>& pcm,
        const int sampleRate)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return false;
        }
        const std::uint32_t dataSize = static_cast<std::uint32_t>(pcm.size());
        stream.write("RIFF", 4);
        WriteLittleEndian(stream, 36U + dataSize, 4);
        stream.write("WAVEfmt ", 8);
        WriteLittleEndian(stream, 16, 4);
        WriteLittleEndian(stream, 1, 2);
        WriteLittleEndian(stream, 1, 2);
        WriteLittleEndian(stream, static_cast<std::uint32_t>(sampleRate), 4);
        WriteLittleEndian(stream, static_cast<std::uint32_t>(sampleRate * 2), 4);
        WriteLittleEndian(stream, 2, 2);
        WriteLittleEndian(stream, 16, 2);
        stream.write("data", 4);
        WriteLittleEndian(stream, dataSize, 4);
        stream.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size()));
        return stream.good();
    }

    std::string Trim(std::string value)
    {
        const std::size_t first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return {};
        }
        const std::size_t last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    std::optional<int> CudaOrdinal(const std::string& device)
    {
        std::string normalized = device;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        if (!normalized.starts_with("cuda:") || normalized.size() <= 5)
        {
            return std::nullopt;
        }
        try
        {
            std::size_t consumed = 0;
            const std::string digits = normalized.substr(5);
            const int ordinal = std::stoi(digits, &consumed);
            return ordinal >= 0 && consumed == digits.size()
                ? std::optional<int>(ordinal)
                : std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

#ifdef _WIN32
    std::wstring Quote(const std::filesystem::path& path)
    {
        std::wstring value = path.wstring();
        std::wstring quoted = L"\"";
        std::size_t backslashes = 0;
        for (const wchar_t character : value)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'\"')
            {
                quoted.append(backslashes * 2 + 1, L'\\');
                quoted.push_back(L'\"');
                backslashes = 0;
                continue;
            }
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
        quoted.append(backslashes * 2, L'\\');
        quoted.push_back(L'\"');
        return quoted;
    }

    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
        {
            return {};
        }
        const int count = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (count <= 0)
        {
            return {};
        }
        std::wstring output(static_cast<std::size_t>(count), L'\0');
        MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            output.data(), count);
        return output;
    }
#endif
}

SpeechRecognitionService::~SpeechRecognitionService()
{
    Shutdown();
}

bool SpeechRecognitionService::Start(
    const speechRecognitionSettings& settings,
    EventHandler handler)
{
    Shutdown();
    {
        std::lock_guard lock(mutex);
        configuration = settings;
        eventHandler = std::move(handler);
    }
    if (!settings.bEnabled)
    {
        Notify({"Disabled", "Speech recognition is off."});
        return true;
    }

    executablePath = ResolveRuntimePath(settings.executable);
    modelPath = ResolveRuntimePath(settings.modelPath);
    if (!std::filesystem::is_regular_file(executablePath) ||
        !std::filesystem::is_regular_file(modelPath))
    {
        available.store(false);
        Notify({"Missing", "whisper.cpp or its speech model is not installed. Run Tools/InstallWhisper.ps1."});
        return false;
    }
    available.store(true);
    if (settings.bUseServer)
    {
        std::string serverError;
        if (serverProcess.Start(settings, serverError))
        {
            Notify({"Starting", "The persistent whisper.cpp service is loading its model."});
            serverWarmupWorker = std::jthread([this](const std::stop_token stopToken)
            {
                std::string error;
                if (!EnsureServerReady(stopToken, error) && !stopToken.stop_requested())
                {
                    Notify({"Fallback", error +
                        " Speech recognition will use the command-line fallback."});
                }
            });
        }
        else
        {
            Notify({"Fallback", serverError + " Speech recognition will use the command-line fallback."});
        }
    }
    else
    {
        Notify({"Ready", "Speech recognition is ready on " +
            (settings.device == "cpu" ? std::string("CPU") : settings.device) + "."});
    }
    SetHandsFreeEnabled(settings.bHandsFree);
    return true;
}

bool SpeechRecognitionService::BeginRecording()
{
    if (!available.load() || handsFreeEnabled.load() || recording.exchange(true) ||
        transcribing.load())
    {
        return false;
    }
    if (recordingWorker.joinable())
    {
        recordingWorker.join();
    }
    discarding.store(false);

    std::error_code error;
    const std::filesystem::path captureDirectory =
        std::filesystem::temp_directory_path(error) / "Revia" / "Speech";
    std::filesystem::create_directories(captureDirectory, error);
    if (error)
    {
        recording.store(false);
        Notify({"Error", "The temporary speech directory could not be created."});
        return false;
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    activeWavePath = captureDirectory / ("capture-" + std::to_string(stamp) + ".wav");
    recordingWorker = std::jthread([this, path = activeWavePath](const std::stop_token stopToken)
    {
        Capture(stopToken, path);
    });
    return true;
}

bool SpeechRecognitionService::EndRecording()
{
    if (!recording.load())
    {
        return false;
    }
    recordingWorker.request_stop();
    if (recordingWorker.joinable())
    {
        recordingWorker.join();
    }
    recording.store(false);
    if (discarding.load() || !std::filesystem::is_regular_file(activeWavePath))
    {
        return false;
    }
    if (transcriptionWorker.joinable())
    {
        transcriptionWorker.join();
    }
    transcriptionCancelled.store(false);
    transcribing.store(true);
    transcriptionWorker = std::jthread(
        [this, path = activeWavePath](const std::stop_token stopToken)
        {
            Transcribe(stopToken, path);
        });
    return true;
}

void SpeechRecognitionService::Cancel()
{
    // Discarding tells an in-flight capture to drop its audio instead of handing it to
    // whisper.cpp, so cancelling never produces a transcript the user did not ask for.
    discarding.store(true);
    transcriptionCancelled.store(true);
    recordingWorker.request_stop();
    transcriptionWorker.request_stop();
    if (transcribing.load())
    {
        // cpp-httplib has no cross-thread cancellation token. Stopping the child makes
        // an in-flight loopback request return immediately; EnsureServerReady restarts
        // it on the next utterance.
        serverProcess.Stop();
        serverReady.store(false);
    }
}

void SpeechRecognitionService::SetHandsFreeEnabled(const bool enabled)
{
    const bool effective = enabled && available.load();
    const bool wasEnabled = handsFreeEnabled.exchange(effective);
    if (!effective)
    {
        if (handsFreeWorker.joinable())
        {
            handsFreeWorker.request_stop();
            handsFreeWorker.join();
        }
        if (available.load() && wasEnabled)
        {
            Notify({"Ready", "Hands-free listening is off."});
        }
        return;
    }
    if (!handsFreeWorker.joinable())
    {
        handsFreeWorker = std::jthread(
            [this](const std::stop_token stopToken) { RunHandsFree(stopToken); });
    }
    Notify({"HandsFree", "Hands-free listening is waiting for speech."});
}

bool SpeechRecognitionService::IsHandsFreeEnabled() const
{
    return handsFreeEnabled.load();
}

void SpeechRecognitionService::SetOutputActive(const bool active)
{
    outputActive.store(active);
}

void SpeechRecognitionService::Shutdown()
{
    handsFreeEnabled.store(false);
    if (handsFreeWorker.joinable())
    {
        handsFreeWorker.request_stop();
    }
    if (serverWarmupWorker.joinable())
    {
        serverWarmupWorker.request_stop();
    }
    Cancel();
    serverProcess.Stop();
    serverReady.store(false);
    if (handsFreeWorker.joinable())
    {
        handsFreeWorker.join();
    }
    if (serverWarmupWorker.joinable())
    {
        serverWarmupWorker.join();
    }
    if (recordingWorker.joinable())
    {
        recordingWorker.join();
    }
    if (transcriptionWorker.joinable())
    {
        transcriptionWorker.join();
    }
    recording.store(false);
    transcribing.store(false);
    available.store(false);
}

bool SpeechRecognitionService::IsAvailable() const
{
    return available.load();
}

bool SpeechRecognitionService::IsRecording() const
{
    return recording.load();
}

std::filesystem::path SpeechRecognitionService::ResolveRuntimePath(
    const std::string& configuredPath)
{
    std::filesystem::path configured(configuredPath);
    if (configured.is_absolute())
    {
        return configured.lexically_normal();
    }
    std::error_code error;
    const std::filesystem::path currentCandidate =
        std::filesystem::absolute(configured, error).lexically_normal();
    if (!error && std::filesystem::exists(currentCandidate))
    {
        return currentCandidate;
    }
#ifdef _WIN32
    std::array<wchar_t, 32768> modulePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (length > 0 && length < modulePath.size())
    {
        const std::filesystem::path executableDirectory =
            std::filesystem::path(std::wstring(modulePath.data(), length)).parent_path();
        for (const std::filesystem::path& root : {
            executableDirectory,
            executableDirectory.parent_path(),
            executableDirectory.parent_path().parent_path()})
        {
            const std::filesystem::path candidate = (root / configured).lexically_normal();
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }
        }
    }
#endif
    return currentCandidate;
}

void SpeechRecognitionService::Capture(
    const std::stop_token stopToken,
    const std::filesystem::path outputPath)
{
#ifdef _WIN32
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = static_cast<DWORD>(configuration.sampleRate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HWAVEIN input = nullptr;
    const MMRESULT opened = waveInOpen(&input, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
    if (opened != MMSYSERR_NOERROR)
    {
        recording.store(false);
        Notify({"Error", "The default microphone could not be opened."});
        return;
    }

    constexpr std::size_t BufferCount = 8;
    constexpr std::size_t BufferBytes = 6400;
    std::array<std::array<char, BufferBytes>, BufferCount> storage{};
    std::array<WAVEHDR, BufferCount> headers{};
    for (std::size_t index = 0; index < BufferCount; ++index)
    {
        headers[index].lpData = storage[index].data();
        headers[index].dwBufferLength = static_cast<DWORD>(BufferBytes);
        waveInPrepareHeader(input, &headers[index], sizeof(WAVEHDR));
        waveInAddBuffer(input, &headers[index], sizeof(WAVEHDR));
    }

    const auto startedAt = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> pcm;
    pcm.reserve(static_cast<std::size_t>(configuration.sampleRate * 2 * 15));
    waveInStart(input);
    Notify({"Recording", "Listening through the default microphone."});
    while (!stopToken.stop_requested())
    {
        for (WAVEHDR& header : headers)
        {
            if ((header.dwFlags & WHDR_DONE) != 0)
            {
                const auto* begin = reinterpret_cast<const std::uint8_t*>(header.lpData);
                pcm.insert(pcm.end(), begin, begin + header.dwBytesRecorded);
                header.dwBytesRecorded = 0;
                header.dwFlags &= ~WHDR_DONE;
                waveInAddBuffer(input, &header, sizeof(WAVEHDR));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    waveInStop(input);
    waveInReset(input);
    for (WAVEHDR& header : headers)
    {
        if (header.dwBytesRecorded > 0)
        {
            const auto* begin = reinterpret_cast<const std::uint8_t*>(header.lpData);
            pcm.insert(pcm.end(), begin, begin + header.dwBytesRecorded);
        }
        waveInUnprepareHeader(input, &header, sizeof(WAVEHDR));
    }
    waveInClose(input);
    recording.store(false);

    if (discarding.load())
    {
        Notify({"Ready", "Listening was cancelled; the audio was discarded.",
            ElapsedMilliseconds(startedAt)});
        return;
    }
    if (pcm.size() < static_cast<std::size_t>(configuration.sampleRate / 2))
    {
        Notify({"Ready", "That was too short to transcribe; speak, then stop listening.",
            ElapsedMilliseconds(startedAt)});
        return;
    }
    if (!WriteWaveFile(outputPath, pcm, configuration.sampleRate))
    {
        Notify({"Error", "The microphone recording could not be saved."});
        return;
    }
    Notify({"Captured", "Audio captured; preparing transcription.",
        ElapsedMilliseconds(startedAt)});
#else
    (void)stopToken;
    (void)outputPath;
    recording.store(false);
    Notify({"Unavailable", "Speech recognition capture is currently implemented for Windows."});
#endif
}

bool SpeechRecognitionService::CaptureHandsFree(
    const std::stop_token stopToken,
    const std::filesystem::path outputPath)
{
#ifndef _WIN32
    (void)stopToken;
    (void)outputPath;
    return false;
#else
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = static_cast<DWORD>(configuration.sampleRate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HWAVEIN input = nullptr;
    if (waveInOpen(&input, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
    {
        Notify({"Error", "The default microphone could not be opened for hands-free listening."});
        return false;
    }
    constexpr std::size_t BufferCount = 8;
    constexpr std::size_t BufferBytes = 1600;
    std::array<std::array<char, BufferBytes>, BufferCount> storage{};
    std::array<WAVEHDR, BufferCount> headers{};
    for (std::size_t index = 0; index < BufferCount; ++index)
    {
        headers[index].lpData = storage[index].data();
        headers[index].dwBufferLength = static_cast<DWORD>(BufferBytes);
        waveInPrepareHeader(input, &headers[index], sizeof(WAVEHDR));
        waveInAddBuffer(input, &headers[index], sizeof(WAVEHDR));
    }

    std::deque<std::vector<std::uint8_t>> preRoll;
    std::vector<std::uint8_t> pcm;
    bool speechStarted = false;
    int qualifyingFrames = 0;
    int silentMilliseconds = 0;
    int capturedMilliseconds = 0;
    double noiseFloor = 150.0;
    waveInStart(input);
    while (!stopToken.stop_requested() && handsFreeEnabled.load())
    {
        if (outputActive.load())
        {
            pcm.clear();
            break;
        }
        bool processed = false;
        for (WAVEHDR& header : headers)
        {
            if ((header.dwFlags & WHDR_DONE) == 0)
            {
                continue;
            }
            processed = true;
            const auto* begin = reinterpret_cast<const std::uint8_t*>(header.lpData);
            const std::size_t bytes = header.dwBytesRecorded;
            double squareSum = 0.0;
            const auto* samples = reinterpret_cast<const std::int16_t*>(header.lpData);
            const std::size_t sampleCount = bytes / sizeof(std::int16_t);
            for (std::size_t index = 0; index < sampleCount; ++index)
            {
                const double sample = static_cast<double>(samples[index]);
                squareSum += sample * sample;
            }
            const double rms = sampleCount == 0 ? 0.0 :
                std::sqrt(squareSum / static_cast<double>(sampleCount));
            const double threshold = std::max<double>(
                configuration.vadEnergyThreshold, noiseFloor * 2.2);
            const bool voiced = rms >= threshold;
            if (!speechStarted)
            {
                noiseFloor = noiseFloor * 0.94 + rms * 0.06;
                preRoll.emplace_back(begin, begin + bytes);
                while (preRoll.size() > 10)
                {
                    preRoll.pop_front();
                }
                qualifyingFrames = voiced ? qualifyingFrames + 1 : 0;
                if (qualifyingFrames >= configuration.vadSpeechFrames)
                {
                    speechStarted = true;
                    recording.store(true);
                    for (const auto& frame : preRoll)
                    {
                        pcm.insert(pcm.end(), frame.begin(), frame.end());
                    }
                    preRoll.clear();
                    RecognitionEvent detected{
                        "SpeechDetected", "Speech detected; listening until the thought ends."};
                    detected.automatic = true;
                    Notify(std::move(detected));
                }
            }
            else
            {
                pcm.insert(pcm.end(), begin, begin + bytes);
                capturedMilliseconds += 50;
                silentMilliseconds = voiced ? 0 : silentMilliseconds + 50;
            }
            header.dwBytesRecorded = 0;
            header.dwFlags &= ~WHDR_DONE;
            waveInAddBuffer(input, &header, sizeof(WAVEHDR));
            if (speechStarted && (silentMilliseconds >= configuration.vadSilenceMs ||
                capturedMilliseconds >= configuration.maximumUtteranceSeconds * 1000))
            {
                break;
            }
        }
        if (speechStarted && (silentMilliseconds >= configuration.vadSilenceMs ||
            capturedMilliseconds >= configuration.maximumUtteranceSeconds * 1000))
        {
            break;
        }
        if (!processed)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    waveInStop(input);
    waveInReset(input);
    for (WAVEHDR& header : headers)
    {
        waveInUnprepareHeader(input, &header, sizeof(WAVEHDR));
    }
    waveInClose(input);
    recording.store(false);
    const std::size_t minimumBytes = static_cast<std::size_t>(configuration.sampleRate * 2) *
        static_cast<std::size_t>(configuration.minimumUtteranceMs) / 1000U;
    if (stopToken.stop_requested() || outputActive.load() || pcm.size() < minimumBytes)
    {
        return false;
    }
    if (!WriteWaveFile(outputPath, pcm, configuration.sampleRate))
    {
        Notify({"Error", "A hands-free speech segment could not be saved."});
        return false;
    }
    RecognitionEvent captured{
        "Captured", "Hands-free speech captured; preparing transcription."};
    captured.automatic = true;
    Notify(std::move(captured));
    return true;
#endif
}

void SpeechRecognitionService::RunHandsFree(const std::stop_token stopToken)
{
    while (!stopToken.stop_requested() && handsFreeEnabled.load())
    {
        if (!available.load() || outputActive.load() || transcribing.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
        }
        std::error_code error;
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path(error) / "Revia" / "Speech";
        std::filesystem::create_directories(directory, error);
        if (error)
        {
            Notify({"Error", "The hands-free speech directory could not be created."});
            return;
        }
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path wavePath =
            directory / ("hands-free-" + std::to_string(stamp) + ".wav");
        if (!CaptureHandsFree(stopToken, wavePath))
        {
            continue;
        }
        transcriptionCancelled.store(false);
        transcribing.store(true);
        Transcribe(stopToken, wavePath, true);
        if (!stopToken.stop_requested() && handsFreeEnabled.load())
        {
            RecognitionEvent waiting{
                "HandsFree", "Hands-free listening is waiting for speech."};
            waiting.automatic = true;
            Notify(std::move(waiting));
        }
    }
}

bool SpeechRecognitionService::EnsureServerReady(
    const std::stop_token stopToken,
    std::string& outError)
{
    outError.clear();
    if (!configuration.bUseServer)
    {
        outError = "The persistent whisper.cpp service is disabled.";
        return false;
    }
    if (!serverProcess.IsRunning())
    {
        if (!serverProcess.Start(configuration, outError))
        {
            return false;
        }
        Notify({"Starting", "The persistent whisper.cpp service is loading its model."});
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(configuration.serverStartupTimeoutSeconds);
    while (!stopToken.stop_requested() && !transcriptionCancelled.load() &&
        std::chrono::steady_clock::now() < deadline)
    {
        httplib::Client client(configuration.serverHost, configuration.serverPort);
        client.set_connection_timeout(1);
        client.set_read_timeout(1);
        if (const auto response = client.Get("/"); response && response->status == 200)
        {
            if (!serverReady.exchange(true))
            {
                Notify({"Ready", "The persistent whisper.cpp model is loaded and ready."});
            }
            return true;
        }
        if (!serverProcess.IsRunning())
        {
            outError = "The persistent whisper.cpp service exited while loading.";
            serverReady.store(false);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    outError = stopToken.stop_requested() || transcriptionCancelled.load()
        ? "Speech transcription was stopped."
        : "The persistent whisper.cpp service did not become ready before its startup timeout.";
    serverReady.store(false);
    return false;
}

std::optional<std::string> SpeechRecognitionService::TranscribeWithServer(
    const std::filesystem::path& wavePath,
    const std::stop_token stopToken,
    std::string& outError)
{
    if (!EnsureServerReady(stopToken, outError) || stopToken.stop_requested() ||
        transcriptionCancelled.load())
    {
        return std::nullopt;
    }
    std::ifstream stream(wavePath, std::ios::binary);
    if (!stream)
    {
        outError = "The captured audio could not be opened for transcription.";
        return std::nullopt;
    }
    const std::string wave(
        (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (wave.empty())
    {
        outError = "The captured audio was empty.";
        return std::nullopt;
    }

    httplib::Client client(configuration.serverHost, configuration.serverPort);
    client.set_connection_timeout(2);
    client.set_read_timeout(configuration.requestTimeoutSeconds);
    client.set_write_timeout(10);
    const httplib::MultipartFormDataItems items{
        {"file", wave, wavePath.filename().string(), "audio/wav"},
        {"response_format", "json", {}, {}},
        {"language", configuration.language, {}, {}}};
    const auto response = client.Post("/inference", items);
    if (stopToken.stop_requested() || transcriptionCancelled.load())
    {
        outError = "Speech transcription was stopped.";
        return std::nullopt;
    }
    if (!response)
    {
        serverReady.store(false);
        outError = "The persistent whisper.cpp service did not answer.";
        return std::nullopt;
    }
    if (response->status < 200 || response->status >= 300)
    {
        serverReady.store(false);
        outError = "The persistent whisper.cpp service returned HTTP " +
            std::to_string(response->status) + ".";
        return std::nullopt;
    }
    try
    {
        const nlohmann::json payload = nlohmann::json::parse(response->body);
        if (!payload.contains("text") || !payload["text"].is_string())
        {
            outError = "The persistent whisper.cpp response did not contain transcript text.";
            return std::nullopt;
        }
        return Trim(payload["text"].get<std::string>());
    }
    catch (const std::exception& exception)
    {
        outError = std::string("The persistent whisper.cpp response was invalid: ") +
            exception.what();
        return std::nullopt;
    }
}

void SpeechRecognitionService::Transcribe(
    const std::stop_token stopToken,
    const std::filesystem::path wavePath,
    const bool automatic)
{
    const auto startedAt = std::chrono::steady_clock::now();
    RecognitionEvent transcribingEvent{
        "Transcribing", "whisper.cpp is transcribing the captured audio."};
    transcribingEvent.automatic = automatic;
    Notify(std::move(transcribingEvent));

    if (configuration.bUseServer)
    {
        std::string serverError;
        if (const std::optional<std::string> transcript =
                TranscribeWithServer(wavePath, stopToken, serverError);
            transcript.has_value())
        {
            std::error_code ignored;
            std::filesystem::remove(wavePath, ignored);
            transcribing.store(false);
            if (transcript->empty() || *transcript == "[BLANK_AUDIO]")
            {
                RecognitionEvent empty{
                    "Ready", "No clear speech was detected.", ElapsedMilliseconds(startedAt)};
                empty.automatic = automatic;
                Notify(std::move(empty));
                return;
            }
            RecognitionEvent completed{
                "Transcript", "Speech transcription completed.", *transcript,
                ElapsedMilliseconds(startedAt)};
            completed.automatic = automatic;
            Notify(std::move(completed));
            return;
        }
        if (stopToken.stop_requested() || transcriptionCancelled.load())
        {
            transcribing.store(false);
            Notify({"Stopped", "Speech transcription was stopped.",
                ElapsedMilliseconds(startedAt)});
            return;
        }
        RecognitionEvent fallback{
            "Fallback", serverError + " Retrying this segment with whisper.cpp CLI."};
        fallback.automatic = automatic;
        Notify(std::move(fallback));
    }

    std::filesystem::path outputBase = wavePath;
    outputBase.replace_extension();
    const std::filesystem::path transcriptPath = outputBase.string() + ".txt";

#ifdef _WIN32
    std::wstring command = Quote(executablePath) + L" -m " + Quote(modelPath) +
        L" -f " + Quote(wavePath) + L" -l " + Utf8ToWide(configuration.language) +
        L" -otxt -of " + Quote(outputBase) + L" --no-timestamps -t " +
        std::to_wstring(configuration.threads);
    const std::optional<int> deviceOrdinal = CudaOrdinal(configuration.device);
    if (!configuration.bUseGpu || configuration.device == "cpu")
    {
        command += L" -ng";
    }
    else if (deviceOrdinal.has_value())
    {
        command += L" -dev " + std::to_wstring(*deviceOrdinal);
    }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring workingDirectory = executablePath.parent_path().wstring();
    const BOOL created = CreateProcessW(
        executablePath.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, workingDirectory.c_str(), &startup, &process);
    if (!created)
    {
        transcribing.store(false);
        Notify({"Error", "whisper.cpp could not be launched."});
        return;
    }
    CloseHandle(process.hThread);
    while (!stopToken.stop_requested() && !transcriptionCancelled.load() &&
        WaitForSingleObject(process.hProcess, 50) == WAIT_TIMEOUT)
    {
    }
    if (stopToken.stop_requested() || transcriptionCancelled.load())
    {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 3000);
        CloseHandle(process.hProcess);
        transcribing.store(false);
        Notify({"Stopped", "Speech transcription was stopped.", ElapsedMilliseconds(startedAt)});
        return;
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    if (exitCode != 0)
    {
        transcribing.store(false);
        Notify({"Error", "whisper.cpp exited before producing a transcript.",
            ElapsedMilliseconds(startedAt)});
        return;
    }
#else
    (void)stopToken;
#endif

    std::ifstream transcriptFile(transcriptPath, std::ios::binary);
    std::ostringstream transcriptBuffer;
    transcriptBuffer << transcriptFile.rdbuf();
    std::string transcript = Trim(transcriptBuffer.str());
    std::error_code ignored;
    std::filesystem::remove(wavePath, ignored);
    std::filesystem::remove(transcriptPath, ignored);
    transcribing.store(false);
    if (transcript.empty() || transcript == "[BLANK_AUDIO]")
    {
        Notify({"Ready", "No clear speech was detected.", ElapsedMilliseconds(startedAt)});
        return;
    }
    RecognitionEvent completed{
        "Transcript", "Speech transcription completed.", transcript,
        ElapsedMilliseconds(startedAt)};
    completed.automatic = automatic;
    Notify(std::move(completed));
}

void SpeechRecognitionService::Notify(RecognitionEvent event) const
{
    EventHandler handler;
    {
        std::lock_guard lock(mutex);
        handler = eventHandler;
    }
    if (handler)
    {
        handler(event);
    }
}

} // namespace revia::speech
