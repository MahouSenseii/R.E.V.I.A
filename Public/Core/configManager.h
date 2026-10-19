#pragma once
#include <string>
#include <vector>

struct aiProfile;
struct appSettings;

class configManager
{
public:
    configManager();
    configManager(std::string settingsFile, std::string profileDirectory, std::string neutralProfileDirectory = {});
    ~configManager();

    // Lays the owner settings overlay at path over the settings file on every load (see
    // revia::core::ApplyOwnerOverrides). An overlay entry the merged settings cannot accept
    // is set aside with a warning, so a stale choice never stops a start; with strict, the
    // load fails instead, which is how the overlay editor validates a change.
    void SetOwnerOverlay(std::string path, bool strict = false);

    bool LoadSettings(appSettings& outSettings) const;
    // outWarnings receives each overlay entry that was ignored or set aside, and why.
    bool LoadSettings(appSettings& outSettings, std::vector<std::string>& outWarnings) const;
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
    std::string neutralProfilePath;
    std::string ownerOverlayPath;
    bool ownerOverlayStrict = false;
};
