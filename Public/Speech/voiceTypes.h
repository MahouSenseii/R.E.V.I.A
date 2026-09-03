#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace revia::speech
{

struct VoicePreset
{
    std::string id;
    std::string name;
    std::string description;
    std::string language = "English";
    std::string referenceText;
    std::string referenceAudioPath;
    std::string createdAt;
};

struct VoiceOperationResult
{
    VoiceOperationResult() = default;
    VoiceOperationResult(
        const bool inputSucceeded,
        std::string inputMessage,
        std::string inputOutputPath,
        const double inputElapsedMilliseconds,
        std::string inputDevice = {},
        std::string inputDeviceName = {},
        std::string inputDtype = {},
        std::string inputWorkerId = {})
        : succeeded(inputSucceeded),
          message(std::move(inputMessage)),
          outputPath(std::move(inputOutputPath)),
          elapsedMilliseconds(inputElapsedMilliseconds),
          device(std::move(inputDevice)),
          deviceName(std::move(inputDeviceName)),
          dtype(std::move(inputDtype)),
          workerId(std::move(inputWorkerId))
    {
    }

    bool succeeded = false;
    std::string message;
    std::string outputPath;
    double elapsedMilliseconds = -1.0;
    std::string device;
    std::string deviceName;
    std::string dtype;
    std::string workerId;
    std::string attentionBackend;
    std::string inputMode;
    // How long this request waited for a worker in QwenTtsPool, from entering
    // AcquireWorker to holding one.
    //
    // This is the queue that exists. The worker also reports how long it waited for
    // its own Python lock, but a worker only ever receives a request once the pool has
    // already chosen it, so that number is structurally near zero and describes
    // nothing -- reading it as pool wait is what made a queue eight phrases deep look
    // like no queue at all.
    double workerPoolWaitMilliseconds = -1.0;
    // The worker's own lock wait, kept separate and named for what it is.
    double workerQueueMilliseconds = -1.0;
    double modelReadyMilliseconds = -1.0;
    double clonePromptMilliseconds = -1.0;
    double generationMilliseconds = -1.0;
    double wavWriteMilliseconds = -1.0;
    double cppResponseMilliseconds = -1.0;
    double audioDurationMilliseconds = -1.0;
    int sampleRate = 0;
    bool clonePromptCached = false;
    bool audioCacheHit = false;
    // What the card was doing while this request ran, reported by the worker that ran
    // it. Per request rather than per session: a slow utterance is explained by the
    // state at the time it was synthesised, not by the state at startup.
    int vramUsedMiB = -1;
    int vramTotalMiB = -1;
    int gpuUtilizationPercent = -1;
    bool modelResident = false;
    // Generation time over generated audio duration. At or above 1.0 synthesis is
    // slower than playback, so a queue of phrases can only grow.
    double realTimeFactor = -1.0;
    // Peak allocation the worker saw during a batched call, and the shape of the batch
    // this clip came out of. Zero phrases means the clip was synthesized on its own.
    int peakVramMiB = -1;
    int batchedPhrases = 0;
    int batchCharacters = 0;
    // Which inference path the worker used for this request, and whether a captured
    // CUDA graph carried it. Reported per request because a worker can fall back to
    // the stock path at load time without anything else changing.
    std::string backend;
    bool cudaGraph = false;
    bool talkerGraph = false;
    // Whether the low-latency module actually installed on this worker, and what the
    // worker said about it.
    //
    // Separate from cudaGraph because they answer different questions and can only be
    // conflated in the direction that flatters the run. Capture is deferred to the
    // first eligible phrase, so at voice-load time a healthy worker reports installed
    // with no graph yet; a worker whose install raised reports neither, and the detail
    // carries the reason.
    bool lowLatencyInstalled = false;
    std::string backendDetail;
    // Normal conversation uses an in-memory RIFF/WAV payload. It remains bounded by
    // qwenMaxBufferedAudioMiB and alive until WinMM finishes asynchronous playback.
    std::vector<std::uint8_t> audioBytes;
};

struct VoiceStudioSnapshot
{
    std::vector<std::string> profiles;
    std::vector<VoicePreset> presets;
    std::string activeProfile;
    std::string assignedPresetId;
    std::unordered_map<std::string, std::string> profileAssignments;
};

} // namespace revia::speech
