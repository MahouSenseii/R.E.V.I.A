#pragma once
#include <string>
#include <vector>

struct aiProfile;
struct appSettings;

class configManager
{
public:
    configManager();
    ~configManager();

    bool LoadSettings(appSettings& outSettings) const;
    bool LoadProfile(const std::string& profileId, aiProfile& outProfile) const;

    // Every profile file on disk, by id, sorted. The desktop profile editor lists these;
    // the runtime only ever loads one of them by name.
    [[nodiscard]] std::vector<std::string> ListProfiles() const;
    // Writes a profile file, creating Config/Profiles when it does not exist. Keys the
    // loader does not understand are preserved from any existing file, so hand-authored
    // fields survive an edit made through the desktop editor.
    bool SaveProfile(const aiProfile& profile, std::string& outError) const;
    // The same containment rule LoadProfile applies, exposed so callers can reject a bad
    // id before building a profile around it.
    [[nodiscard]] static bool IsSafeProfileId(const std::string& profileId);

private:

    std::string settingsPath = "Config/settings.json";
    std::string profilePath = "Config/Profiles";
};
