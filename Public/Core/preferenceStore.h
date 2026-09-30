#pragma once


#include "Core/appSettings.h"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace revia::core
{

enum class PreferenceType
{
    Boolean,
    Integer,
    Decimal,
    Text
};

// One setting a user is allowed to change and keep.
struct PreferenceKey
{
    std::string name;
    PreferenceType type = PreferenceType::Boolean;
    std::string description;
    double minimum = 0.0;
    double maximum = 0.0;
    // Allowed values for a Text preference; empty means any non-empty string.
    std::vector<std::string> allowed;
};

struct PreferenceResult
{
    bool succeeded = false;
    std::string message;
};

// Persists a fixed allowlist of non-authority preferences under RuntimeData; unknown keys are refused.
// Capability scope, execution/risk, internet, screen capture and ambient perception cannot be changed here.
class PreferenceStore
{
public:
    explicit PreferenceStore(std::filesystem::path path = "RuntimeData/Preferences/preferences.json");

    // The complete set of writable settings. Anything not here is refused.
    [[nodiscard]] static const std::vector<PreferenceKey>& Writable();
    [[nodiscard]] static const PreferenceKey* Find(const std::string& name);
    // Names that are refused with an explanation rather than a generic "unknown key",
    // because being told why authority is not a preference is more useful than a typo
    // message that invites another guess.
    [[nodiscard]] static bool IsAuthoritySetting(const std::string& name);

    PreferenceResult Set(const std::string& name, const std::string& value);
    PreferenceResult Clear(const std::string& name);
    [[nodiscard]] std::map<std::string, std::string> Load() const;
    // Overlays stored preferences onto freshly loaded settings. Called after the config
    // file is parsed and validated, so a stored value can never bypass validation.
    void Apply(appSettings& settings) const;
    [[nodiscard]] std::string Describe() const;

private:
    [[nodiscard]] bool Write(const std::map<std::string, std::string>& values) const;

    std::filesystem::path storePath;
};

} // namespace revia::core
