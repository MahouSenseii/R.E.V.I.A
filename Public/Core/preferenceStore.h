#pragma once

#include "Library/structLibrary.h"

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

// Durable, non-authority settings.
//
// The load-bearing property is what this **cannot** write. Approved roots, approved
// applications, control scopes, execution mode, risk ceilings, internet access, screen
// capture, and ambient perception are all absent from the writable table and are refused
// by name, because a preference command that could widen authority is an authority
// escalation wearing a convenient interface. Those live in capabilities.json behind
// CapabilityEditor, or in settings.json behind a deliberate edit.
//
// The table is a fixed allowlist compiled into the binary: an unknown key is refused
// rather than passed through, so the set of things a preference can reach cannot grow by
// accident or by a model writing a plausible-looking name.
//
// Preferences live under RuntimeData for the same reason capabilities do -- rebuilding
// the project must not silently restore a value the user deliberately changed.
class PreferenceStore
{
public:
    explicit PreferenceStore(
        std::filesystem::path path = "RuntimeData/Preferences/preferences.json");

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
