#pragma once

#include "Speech/voiceTypes.h"

#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace revia::speech
{

class VoicePresetStore
{
public:
    explicit VoicePresetStore(std::filesystem::path root = "RuntimeData/Voices");

    void SetRoot(std::filesystem::path root);
    std::filesystem::path Root() const;
    std::vector<VoicePreset> List() const;
    std::optional<VoicePreset> Find(const std::string& presetId) const;
    bool Save(const VoicePreset& preset, std::string& outError);
    bool Assign(const std::string& profileId, const std::string& presetId, std::string& outError);
    std::string AssignedPresetId(const std::string& profileId) const;

    static bool IsSafeId(const std::string& value);

private:
    bool LoadDocument(nlohmann::json& outDocument, std::string& outError) const;
    bool SaveDocument(const nlohmann::json& document, std::string& outError) const;

    mutable std::mutex mutex;
    std::filesystem::path rootDirectory;
};

} // namespace revia::speech
