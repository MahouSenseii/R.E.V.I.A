#include "Speech/speechRecognitionService.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
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
    Notify({"Ready", "Push-to-talk speech recognition is ready."});
    return true;
}

bool SpeechRecognitionService::BeginRecording()
{
    if (!available.load() || recording.exchange(true) || transcribing.load())
    {
        return false;
    }
    if (recordingWorker.joinable())
    {
        recordingWorker.join();
    }

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
    if (!std::filesystem::is_regular_file(activeWavePath))
    {
        return false;
    }
    if (transcriptionWorker.joinable())
    {
        transcriptionWorker.join();
    }
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
    recordingWorker.request_stop();
    transcriptionWorker.request_stop();
}

void SpeechRecognitionService::Shutdown()
{
    Cancel();
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

    if (pcm.size() < static_cast<std::size_t>(configuration.sampleRate / 2))
    {
        Notify({"Ready", "Recording was too short; hold the button while speaking.",
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

void SpeechRecognitionService::Transcribe(
    const std::stop_token stopToken,
    const std::filesystem::path wavePath)
{
    const auto startedAt = std::chrono::steady_clock::now();
    Notify({"Transcribing", "whisper.cpp is transcribing the captured audio."});
    std::filesystem::path outputBase = wavePath;
    outputBase.replace_extension();
    const std::filesystem::path transcriptPath = outputBase.string() + ".txt";

#ifdef _WIN32
    std::wstring command = Quote(executablePath) + L" -m " + Quote(modelPath) +
        L" -f " + Quote(wavePath) + L" -l " + Utf8ToWide(configuration.language) +
        L" -otxt -of " + Quote(outputBase) + L" --no-timestamps -t " +
        std::to_wstring(configuration.threads);
    if (!configuration.bUseGpu)
    {
        command += L" -ng";
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
    while (!stopToken.stop_requested() && WaitForSingleObject(process.hProcess, 50) == WAIT_TIMEOUT)
    {
    }
    if (stopToken.stop_requested())
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
    Notify({"Transcript", "Speech transcription completed.", transcript,
        ElapsedMilliseconds(startedAt)});
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
