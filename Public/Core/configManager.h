#pragma once
#include <string>

struct aiProfile;
struct appSettings;

class configManager
{
public:
    configManager();
    ~configManager();

    bool LoadSettings(appSettings& outSettings) const;
    bool LoadProfile(const std::string& profileId, aiProfile& outProfile) const;

private:

    std::string settingsPath = "Config/settings.json";
    std::string profilePath = "Config/Profiles";
};
