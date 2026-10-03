#pragma once

#include "Speech/systemCue.h"
#include "Speech/voiceTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace revia::speech
{

class SystemCueBank
{
public:
    [[nodiscard]] static std::optional<SystemCueBank> ForVoice(const std::filesystem::path& root,
        const std::string& profileId, const VoicePreset& preset, unsigned phraseVersion = SystemCuePhraseVersion);
    [[nodiscard]] const std::string& Key() const;
    [[nodiscard]] const std::filesystem::path& Directory() const;
    [[nodiscard]] std::filesystem::path Clip(SystemCueKind kind) const;
    [[nodiscard]] std::vector<SystemCueKind> MissingKinds() const;
    [[nodiscard]] std::filesystem::path ScratchPath(SystemCueKind kind) const;
    // Only the requested validated scratch artifact may replace the published clip.
    bool Publish(SystemCueKind kind, const std::filesystem::path& scratch) const;

private:
    std::string key;
    std::filesystem::path directory;
};

} // namespace revia::speech
