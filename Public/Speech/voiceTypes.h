#pragma once

#include <string>
#include <unordered_map>
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
    bool succeeded = false;
    std::string message;
    std::string outputPath;
    double elapsedMilliseconds = -1.0;
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
