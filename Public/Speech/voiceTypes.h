#pragma once

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
