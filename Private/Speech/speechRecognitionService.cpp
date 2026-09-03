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
#include <iomanip>
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

    // The waveIn result codes worth telling a person apart, in their words rather than
    // as a number. "Allocated" and "bad device id" call for completely different
    // actions, and both used to arrive as the same sentence.
    std::string DescribeWaveInResult(const unsigned int result)
    {
#ifdef _WIN32
        switch (result)
        {
            case MMSYSERR_NOERROR: return "opened";
            case MMSYSERR_BADDEVICEID:
                return "no such recording device";
            case MMSYSERR_ALLOCATED:
                return "the device is already in use by another application";
            case MMSYSERR_NODRIVER:
                return "no driver is present for the device";
            case MMSYSERR_NOMEM:
                return "the driver could not allocate memory";
            case WAVERR_BADFORMAT:
                return "the device does not support 16-bit mono at this sample rate";
            case MMSYSERR_INVALHANDLE:
                return "invalid device handle";
            default: break;
        }
#endif
        return "MMSYSERR " + std::to_string(result);
    }

    // Signal strength of a 16-bit mono PCM buffer, as RMS and peak in 0..1.
    //
    // Both, because they disagree in the informative case: a muted input reads zero on
    // each, while a microphone picking up only room tone has a small RMS and a peak
    // that never approaches full scale. One number could not tell those apart.
    struct SignalLevel
    {
        double rms = 0.0;
        double peak = 0.0;
    };

    SignalLevel MeasureSignal(const std::vector<std::uint8_t>& pcm)
    {
        SignalLevel level;
        const std::size_t samples = pcm.size() / 2;
        if (samples == 0) return level;
        double sumOfSquares = 0.0;
        for (std::size_t index = 0; index < samples; ++index)
        {
            const auto low = static_cast<std::uint16_t>(pcm[index * 2]);
            const auto high = static_cast<std::uint16_t>(pcm[index * 2 + 1]);
            const auto sample = static_cast<std::int16_t>(
                static_cast<std::uint16_t>(low | (high << 8U)));
            const double normalized = static_cast<double>(sample) / 32768.0;
            sumOfSquares += normalized * normalized;
            level.peak = std::max(level.peak, std::abs(normalized));
        }
        level.rms = std::sqrt(sumOfSquares / static_cast<double>(samples));
        return level;
    }

    // Below this RMS the capture is indistinguishable from a disconnected or muted
    // input. Chosen to sit under room tone on a normal desktop microphone rather than
    // at zero, because a device that is capturing nothing usually still returns
    // buffers -- it returns buffers of silence, which looks like success.
    constexpr double NoSignalRmsThreshold = 0.0015;

    // Three decimals. A level is read to judge "is anything arriving", and the full
    // double is noise in a sentence a person is meant to act on.
    std::string FormatLevel(const double value)
    {
        std::ostringstream formatted;
        formatted << std::fixed << std::setprecision(3) << value;
        return formatted.str();
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

MicrophoneSelection SelectMicrophone(
    const std::vector<MicrophoneDevice>& devices,
    const std::string& configuredName)
{
    MicrophoneSelection selection;
    const auto trimmed = [](std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string();
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    };
    const std::string wanted = trimmed(configuredName);
    const auto equalsIgnoringCase = [](const std::string& left, const std::string& right)
    {
        if (left.size() != right.size()) return false;
        for (std::size_t index = 0; index < left.size(); ++index)
        {
            if (std::tolower(static_cast<unsigned char>(left[index])) !=
                std::tolower(static_cast<unsigned char>(right[index])))
            {
                return false;
            }
        }
        return true;
    };

    if (wanted.empty() || equalsIgnoringCase(wanted, "default"))
    {
        selection.deviceId = -1;
        selection.name = "Default";
        selection.report = "Using the Windows default recording device.";
        return selection;
    }
    for (const MicrophoneDevice& device : devices)
    {
        if (equalsIgnoringCase(device.name, wanted))
        {
            selection.deviceId = device.id;
            selection.name = device.name;
            selection.report = "Using the selected microphone: " + device.name + ".";
            return selection;
        }
    }
    // Named, and not here. Falling back is the kinder behaviour, but it is reported
    // every time rather than absorbed -- the failure the user asked to never happen is
    // a different microphone being used without anyone being told.
    selection.deviceId = -1;
    selection.name = "Default";
    selection.fellBackToDefault = true;
    selection.report = "The selected microphone \"" + wanted +
        "\" is not connected. Using the Windows default instead.";
    return selection;
}

std::vector<MicrophoneDevice> SpeechRecognitionService::EnumerateMicrophones()
{
    std::vector<MicrophoneDevice> devices;
#ifdef _WIN32
    const UINT count = waveInGetNumDevs();
    devices.reserve(count);
    for (UINT index = 0; index < count; ++index)
    {
        WAVEINCAPSW capabilities{};
        if (waveInGetDevCapsW(index, &capabilities, sizeof(capabilities)) !=
            MMSYSERR_NOERROR)
        {
            continue;
        }
        const int length = WideCharToMultiByte(
            CP_UTF8, 0, capabilities.szPname, -1, nullptr, 0, nullptr, nullptr);
        if (length <= 1) continue;
        std::string name(static_cast<std::size_t>(length - 1), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, capabilities.szPname, -1, name.data(), length, nullptr, nullptr);
        devices.push_back({static_cast<int>(index), name});
    }
#endif
    return devices;
}

MicrophoneSelection SpeechRecognitionService::ResolveMicrophone() const
{
    std::string configured;
    {
        std::lock_guard lock(mutex);
        configured = configuration.microphoneDevice;
    }
    return SelectMicrophone(EnumerateMicrophones(), configured);
}

void SpeechRecognitionService::SetMicrophoneDevice(const std::string& deviceName)
{
    {
        std::lock_guard lock(mutex);
        configuration.microphoneDevice = deviceName;
    }
    const MicrophoneSelection selection = ResolveMicrophone();
    Notify({selection.fellBackToDefault ? "Error" : "Ready",
        selection.fellBackToDefault
            ? "Microphone error: " + selection.report
            : selection.report});
}

std::string SpeechRecognitionService::MicrophoneDeviceSetting() const
{
    std::lock_guard lock(mutex);
    return configuration.microphoneDevice;
}

std::string MicrophoneAttempt::Summary() const
{
    const auto yesNo = [](const bool value) { return value ? "yes" : "no"; };
    return std::string("requested=yes started=") + yesNo(started) +
        " recognizer_available=" + yesNo(recognizerAvailable) +
        " hands_free=" + yesNo(handsFree) +
        " already_recording=" + yesNo(alreadyRecording) +
        " transcribing=" + yesNo(transcribing) +
        " device=" + device +
        (reason.empty() ? std::string() : " reason=" + reason);
}

bool SpeechRecognitionService::BeginRecording()
{
    return BeginRecordingDiagnosed().started;
}

MicrophoneAttempt SpeechRecognitionService::BeginRecordingDiagnosed()
{
    MicrophoneAttempt attempt;
    attempt.recognizerAvailable = available.load();
    attempt.handsFree = handsFreeEnabled.load();
    attempt.alreadyRecording = recording.load();
    attempt.transcribing = transcribing.load();
    const MicrophoneSelection selection = ResolveMicrophone();
    attempt.device = selection.name;

    // Every refusal is decided before anything is claimed.
    //
    // The previous version tested `recording.exchange(true)` inside the short-circuit
    // chain and then went on to test `transcribing`. When a Listen press arrived while
    // whisper.cpp was still working on the last utterance -- which is precisely when a
    // person presses it again -- the exchange had already latched `recording` to true
    // and the transcribing test then returned false without unwinding it. Nothing else
    // ever cleared the flag, so from that moment on every press failed at the exchange
    // and the microphone was dead for the rest of the session. That is the whole of
    // "the Listen button does not reliably work".
    if (!attempt.recognizerAvailable)
    {
        attempt.reason = "Speech recognition is not available. "
            "whisper.cpp or its model may not be installed.";
        return attempt;
    }
    if (attempt.handsFree)
    {
        attempt.reason = "Hands-free mode owns the microphone. "
            "Turn hands-free off to use the Listen button.";
        return attempt;
    }
    if (testing.load())
    {
        attempt.reason = "A microphone test is using the device. It finishes shortly.";
        return attempt;
    }
    if (attempt.transcribing)
    {
        attempt.reason = "The previous recording is still being transcribed.";
        return attempt;
    }
    // Claimed last, and only once nothing can refuse afterwards. exchange still guards
    // two threads racing to start; it just no longer sits in front of a test that can
    // fail behind it.
    if (recording.exchange(true))
    {
        attempt.alreadyRecording = true;
        attempt.reason = "A recording is already running.";
        return attempt;
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
        attempt.reason = "The temporary speech directory could not be created.";
        Notify({"Error", "Microphone error: " + attempt.reason});
        return attempt;
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    activeWavePath = captureDirectory / ("capture-" + std::to_string(stamp) + ".wav");
    recordingWorker = std::jthread([this, path = activeWavePath](const std::stop_token stopToken)
    {
        Capture(stopToken, path);
    });
    attempt.started = true;
    attempt.reason = selection.fellBackToDefault ? selection.report : "";
    return attempt;
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

MicrophoneTestResult SpeechRecognitionService::TestMicrophone(
    const int seconds,
    const bool transcribe)
{
    MicrophoneTestResult result;
    const MicrophoneSelection selection = ResolveMicrophone();
    result.deviceId = selection.deviceId;
    result.deviceName = selection.name;

    if (recording.load() || transcribing.load() || handsFreeEnabled.load())
    {
        result.status = "device unavailable";
        result.message = handsFreeEnabled.load()
            ? "Hands-free mode is using the microphone. Turn it off to run a test."
            : "Revia is already recording or transcribing. Try again in a moment.";
        Notify({"TestFailed", result.message});
        return result;
    }
    if (testing.exchange(true))
    {
        result.status = "device unavailable";
        result.message = "A microphone test is already running.";
        Notify({"TestFailed", result.message});
        return result;
    }
    // The flag is released however this exits, including the early returns below.
    struct TestingGuard
    {
        std::atomic<bool>& flag;
        ~TestingGuard() { flag.store(false); }
    } testingGuard{testing};

    if (selection.fellBackToDefault)
    {
        Notify({"TestProgress", selection.report});
    }

#ifdef _WIN32
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = static_cast<DWORD>(configuration.sampleRate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    const UINT deviceId = selection.deviceId < 0
        ? static_cast<UINT>(WAVE_MAPPER)
        : static_cast<UINT>(selection.deviceId);
    HWAVEIN input = nullptr;
    const MMRESULT opened = waveInOpen(
        &input, deviceId, &format, 0, 0, CALLBACK_NULL);
    if (opened != MMSYSERR_NOERROR)
    {
        result.status = "device unavailable";
        result.message = selection.name + " could not be opened (" +
            DescribeWaveInResult(opened) + ").";
        Notify({"TestFailed", "Microphone test: " + result.message});
        return result;
    }
    result.deviceOpened = true;
    Notify({"TestProgress", "Microphone test: " + selection.name + " opened."});

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
    const auto duration = std::chrono::seconds(std::clamp(seconds, 1, 15));
    std::vector<std::uint8_t> pcm;
    waveInStart(input);
    Notify({"TestRecording",
        "Microphone test: recording for " +
        std::to_string(duration.count()) + "s. Say something."});
    while (std::chrono::steady_clock::now() - startedAt < duration)
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

    result.capturedBytes = pcm.size();
    result.capturedMilliseconds = ElapsedMilliseconds(startedAt);
    const SignalLevel level = MeasureSignal(pcm);
    result.rmsLevel = level.rms;
    result.peakLevel = level.peak;
    result.audioReceived = !pcm.empty();
    result.signalPresent = level.rms >= NoSignalRmsThreshold;

    if (!result.audioReceived)
    {
        result.status = "no signal";
        result.message = selection.name +
            " opened but delivered no audio at all. It may be muted or held by "
            "another application.";
        Notify({"TestFailed", "Microphone test: " + result.message});
        return result;
    }
    if (!result.signalPresent)
    {
        result.status = "no signal";
        result.message = selection.name + " is recording silence (RMS " +
            FormatLevel(level.rms) + "). Check that it is unmuted and its input level "
            "is up.";
        Notify({"TestFailed", "Microphone test: " + result.message});
        return result;
    }

    result.status = "audio received";
    Notify({"TestProgress", "Microphone test: audio received (RMS " +
        FormatLevel(level.rms) + ", peak " + FormatLevel(level.peak) + ")."});

    if (!transcribe)
    {
        result.succeeded = true;
        result.message = selection.name + " is working. Captured " +
            std::to_string(pcm.size()) + " bytes at RMS " + FormatLevel(level.rms) + ".";
        Notify({"TestPassed", "Microphone test: " + result.message});
        return result;
    }

    std::error_code error;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path(error) / "Revia" / "Speech";
    std::filesystem::create_directories(directory, error);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path testPath =
        directory / ("microphone-test-" + std::to_string(stamp) + ".wav");
    if (error || !WriteWaveFile(testPath, pcm, configuration.sampleRate))
    {
        // The audio was good; only the optional half failed. Reporting success on the
        // device is the accurate answer, because that is what the test was asked.
        result.succeeded = true;
        result.status = "audio received";
        result.message = selection.name +
            " is working, but the test clip could not be saved for transcription.";
        Notify({"TestPassed", "Microphone test: " + result.message});
        return result;
    }

    result.status = "transcribing";
    Notify({"TestProgress", "Microphone test: whisper.cpp is transcribing the clip."});
    std::string transcriptionError;
    std::optional<std::string> transcript;
    if (configuration.bUseServer)
    {
        transcript = TranscribeWithServer(testPath, {}, transcriptionError);
    }
    else
    {
        transcriptionError =
            "the persistent whisper.cpp service is disabled for this profile";
    }
    std::filesystem::remove(testPath, error);

    result.succeeded = true;
    if (!transcript.has_value())
    {
        result.status = "audio received";
        result.message = selection.name + " is working. Transcription was not "
            "available for the test clip: " + transcriptionError;
        Notify({"TestPassed", "Microphone test: " + result.message});
        return result;
    }
    // Returned to the caller and shown. Deliberately not routed through Notify's
    // "Transcript" phase, which the shell puts in the message box and may auto-send: a
    // test must never become something Revia was told.
    result.transcript = *transcript == "[BLANK_AUDIO]" ? std::string() : *transcript;
    result.status = "success";
    result.message = result.transcript.empty()
        ? selection.name + " is working, but no clear speech was recognised."
        : selection.name + " is working. Heard: \"" + result.transcript + "\"";
    Notify({"TestPassed", "Microphone test: " + result.message});
    return result;
#else
    (void)seconds;
    (void)transcribe;
    result.status = "failed";
    result.message = "Microphone testing is currently implemented for Windows.";
    Notify({"TestFailed", result.message});
    return result;
#endif
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

    const MicrophoneSelection selection = ResolveMicrophone();
    const UINT deviceId = selection.deviceId < 0
        ? static_cast<UINT>(WAVE_MAPPER)
        : static_cast<UINT>(selection.deviceId);
    if (selection.fellBackToDefault)
    {
        Notify({"Error", "Microphone error: " + selection.report});
    }

    HWAVEIN input = nullptr;
    const MMRESULT opened = waveInOpen(&input, deviceId, &format, 0, 0, CALLBACK_NULL);
    if (opened != MMSYSERR_NOERROR)
    {
        recording.store(false);
        // The result code is in the message on purpose. "Could not be opened" is the
        // same sentence whether the device is in use by another application, muted at
        // the OS level, or absent, and those need different things done about them.
        Notify({"Error", "Microphone error: " + selection.name +
            " could not be opened (" + DescribeWaveInResult(opened) + ")."});
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
    Notify({"Recording", "Listening through " + selection.name + "."});
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

    const double captureMilliseconds = ElapsedMilliseconds(startedAt);
    const SignalLevel level = MeasureSignal(pcm);
    // The capture facts, once, whatever happens next. Bytes and duration together are
    // what separate "the device gave us nothing" from "the device gave us silence",
    // and neither was previously recoverable from the log.
    Notify({"Diagnostics",
        "capture device=" + selection.name +
        " open=" + DescribeWaveInResult(opened) +
        " bytes=" + std::to_string(pcm.size()) +
        " duration_ms=" + std::to_string(static_cast<long long>(captureMilliseconds)) +
        " rms=" + std::to_string(level.rms) +
        " peak=" + std::to_string(level.peak) +
        " wav=" + outputPath.string(),
        captureMilliseconds});

    if (discarding.load())
    {
        Notify({"Ready", "Listening was cancelled; the audio was discarded.",
            captureMilliseconds});
        return;
    }
    if (pcm.empty())
    {
        Notify({"Error", "Microphone error: " + selection.name +
            " opened but delivered no audio. Check that the device is not muted or "
            "in use by another application.", captureMilliseconds});
        return;
    }
    if (pcm.size() < static_cast<std::size_t>(configuration.sampleRate / 2))
    {
        Notify({"Ready", "That was too short to transcribe; speak, then stop listening.",
            captureMilliseconds});
        return;
    }
    if (level.rms < NoSignalRmsThreshold)
    {
        // Not an error: the recording is real and the user may simply not have spoken.
        // But saying so beats handing whisper.cpp silence and reporting an empty
        // transcript, which reads as the microphone having failed.
        Notify({"Ready", "That recording was silent (" + selection.name +
            "). Check the microphone is unmuted and its level is up, then try again.",
            captureMilliseconds});
        return;
    }
    if (!WriteWaveFile(outputPath, pcm, configuration.sampleRate))
    {
        Notify({"Error", "Microphone error: the recording could not be saved to " +
            outputPath.string() + "."});
        return;
    }
    Notify({"Captured", "Audio captured; preparing transcription.",
        captureMilliseconds});
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
