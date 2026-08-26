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
