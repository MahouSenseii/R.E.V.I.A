#pragma once

#include "Library/structLibrary.h"
#include "Speech/whisperServerProcess.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace revia::speech
{

struct RecognitionEvent
{
    RecognitionEvent() = default;
    RecognitionEvent(std::string inputPhase, std::string inputDetail, double inputElapsed = -1.0)
        : phase(std::move(inputPhase)), detail(std::move(inputDetail)),
          elapsedMilliseconds(inputElapsed)
    {
    }
    RecognitionEvent(
        std::string inputPhase,
        std::string inputDetail,
        std::string inputTranscript,
        double inputElapsed = -1.0)
        : phase(std::move(inputPhase)), detail(std::move(inputDetail)),
          transcript(std::move(inputTranscript)), elapsedMilliseconds(inputElapsed)
    {
    }

    std::string phase;
    std::string detail;
    std::string transcript;
    double elapsedMilliseconds = -1.0;
    bool automatic = false;
};

class SpeechRecognitionService
{
public:
    using EventHandler = std::function<void(const RecognitionEvent&)>;

    SpeechRecognitionService() = default;
    ~SpeechRecognitionService();

    SpeechRecognitionService(const SpeechRecognitionService&) = delete;
    SpeechRecognitionService& operator=(const SpeechRecognitionService&) = delete;

    bool Start(const speechRecognitionSettings& settings, EventHandler handler);
    bool BeginRecording();
    bool EndRecording();
    void SetHandsFreeEnabled(bool enabled);
    [[nodiscard]] bool IsHandsFreeEnabled() const;
    void SetOutputActive(bool active);
    void Cancel();
    void Shutdown();
    bool IsAvailable() const;
    bool IsRecording() const;

    static std::filesystem::path ResolveRuntimePath(const std::string& configuredPath);

private:
    void Capture(std::stop_token stopToken, std::filesystem::path outputPath);
    bool CaptureHandsFree(std::stop_token stopToken, std::filesystem::path outputPath);
    void RunHandsFree(std::stop_token stopToken);
    void Transcribe(
        std::stop_token stopToken,
        std::filesystem::path wavePath,
        bool automatic = false);
    bool EnsureServerReady(std::stop_token stopToken, std::string& outError);
    std::optional<std::string> TranscribeWithServer(
        const std::filesystem::path& wavePath,
        std::stop_token stopToken,
        std::string& outError);
    void Notify(RecognitionEvent event) const;

    mutable std::mutex mutex;
    speechRecognitionSettings configuration;
    EventHandler eventHandler;
    std::filesystem::path executablePath;
    std::filesystem::path modelPath;
    std::filesystem::path activeWavePath;
    std::jthread recordingWorker;
    std::jthread transcriptionWorker;
    std::jthread handsFreeWorker;
    std::jthread serverWarmupWorker;
    WhisperServerProcess serverProcess;
    std::atomic<bool> available = false;
    std::atomic<bool> recording = false;
    std::atomic<bool> transcribing = false;
    std::atomic<bool> transcriptionCancelled = false;
    std::atomic<bool> discarding = false;
    std::atomic<bool> handsFreeEnabled = false;
    std::atomic<bool> outputActive = false;
    std::atomic<bool> serverReady = false;
};

} // namespace revia::speech
