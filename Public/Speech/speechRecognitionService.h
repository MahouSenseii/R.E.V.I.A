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
#include <vector>

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

// One Windows recording device, as the operating system reports it.
struct MicrophoneDevice
{
    // The waveIn ordinal. -1 is the system default (WAVE_MAPPER), which is a real
    // choice rather than an absent one: it follows whatever the user picks in Windows.
    int id = -1;
    std::string name;
};

// What a configured microphone name resolved to.
struct MicrophoneSelection
{
    int deviceId = -1;
    std::string name = "Default";
    // True when a device was named in settings and is not present now. The capture
    // still proceeds on the default, because a companion that goes deaf when a USB
    // headset is unplugged is worse than one that says it moved -- but it is never
    // silent about having moved.
    bool fellBackToDefault = false;
    // Always populated, in plain language, for the log and the shell.
    std::string report;
};

// Resolves a configured microphone name against the devices actually present.
//
// Free and pure so the fallback behaviour is testable without a sound card. Matching is
// by name rather than by ordinal on purpose: waveIn ordinals renumber when any device
// is added or removed, so a saved index silently becomes a different microphone, which
// is the one failure the user asked never to happen quietly.
[[nodiscard]] MicrophoneSelection SelectMicrophone(
    const std::vector<MicrophoneDevice>& devices,
    const std::string& configuredName);

// Why a Listen press did or did not become a recording.
//
// Every field the shell needs to explain a failure, filled in whether or not the
// attempt succeeded. The previous code answered this question with a bare false, which
// is why a microphone that could not open was indistinguishable from a button that had
// not been wired up.
struct MicrophoneAttempt
{
    bool started = false;
    bool recognizerAvailable = false;
    bool handsFree = false;
    bool alreadyRecording = false;
    bool transcribing = false;
    std::string device = "Default";
    // Plain sentence, safe to show a person as-is.
    std::string reason;

    // One line for the log, in the order a reader asks the questions.
    [[nodiscard]] std::string Summary() const;
};

// What a microphone test found. Never a conversation turn: the transcript is returned
// to the caller for display and goes nowhere near the input arbiter.
struct MicrophoneTestResult
{
    bool deviceOpened = false;
    bool audioReceived = false;
    bool signalPresent = false;
    bool succeeded = false;
    int deviceId = -1;
    std::string deviceName;
    std::size_t capturedBytes = 0;
    double capturedMilliseconds = 0.0;
    // Root mean square of the captured samples, 0..1. The number behind "no signal",
    // reported so a microphone that is merely quiet is distinguishable from one that is
    // muted or capturing digital silence.
    double rmsLevel = 0.0;
    double peakLevel = 0.0;
    std::string transcript;
    std::string status;
    std::string message;
};

struct MicrophoneTestAccess;

class SpeechRecognitionService
{
    // The refusal states are reached from hardware and worker threads, and the defect
    // worth a regression test -- a refusal leaving the recording flag latched -- is a
    // property of the gate rather than of the microphone. The tests set the flags
    // directly instead of contriving a real transcription to collide with.
    friend struct MicrophoneTestAccess;

public:
    using EventHandler = std::function<void(const RecognitionEvent&)>;

    // The recording devices Windows currently reports, default first. Empty on a
    // machine with no capture hardware, which is a valid answer and not an error.
    [[nodiscard]] static std::vector<MicrophoneDevice> EnumerateMicrophones();

    SpeechRecognitionService() = default;
    ~SpeechRecognitionService();

    SpeechRecognitionService(const SpeechRecognitionService&) = delete;
    SpeechRecognitionService& operator=(const SpeechRecognitionService&) = delete;

    bool Start(const speechRecognitionSettings& settings, EventHandler handler);
    bool BeginRecording();
    // The same call, with the reasoning kept. BeginRecording() is this with the
    // explanation discarded.
    MicrophoneAttempt BeginRecordingDiagnosed();
    bool EndRecording();
    // Opens the selected device, records for a few seconds, measures the signal, and
    // optionally transcribes it. Synchronous and self-contained: it refuses while a
    // real recording or transcription is in flight rather than competing for the
    // device, and its transcript is never submitted as conversation.
    [[nodiscard]] MicrophoneTestResult TestMicrophone(
        int seconds = 3, bool transcribe = true);
    // Changes the capture device for subsequent recordings. Takes effect on the next
    // Listen press; an in-flight capture keeps the device it opened.
    void SetMicrophoneDevice(const std::string& deviceName);
    [[nodiscard]] std::string MicrophoneDeviceSetting() const;
    // What the configured name resolves to right now, including whether it is missing.
    [[nodiscard]] MicrophoneSelection ResolveMicrophone() const;
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
    // Set for the duration of a microphone test so a Listen press and a test cannot
    // both hold the device. Separate from `recording` because a test is not a
    // recording and must not look like one to the shell.
    std::atomic<bool> testing = false;
};

} // namespace revia::speech
