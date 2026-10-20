#pragma once

#include "Core/settingsFileKeys.h"

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct appSettings;

namespace revia::core
{

// The allowlist entry for a dotted owner key, or null when Settings may not change it. For
// "mcp.servers[<id>].enabled" and ".requireConfirmation" the server id is returned through
// outServerId; it is cleared for every other key.
[[nodiscard]] const OwnerKey* FindOwnerKey(std::string_view key, std::string* outServerId = nullptr);

// Whether a load would apply this overlay entry: an allowlisted key holding a value of its kind
// (a boolean, or one of the choice's values). Everything else is ignored by every load, so
// nothing needs to copy it, however large or deeply nested it is.
[[nodiscard]] bool IsAcceptedOwnerValue(std::string_view key, const nlohmann::json& value);

// What an owner key holds in loaded settings: a boolean, or the text of a choice. Empty for a
// key that is not allowlisted, or an MCP key naming a server the settings do not have.
[[nodiscard]] std::optional<nlohmann::json> OwnerSettingValue(const appSettings& settings, std::string_view key);

// What reading the owner overlay found.
enum class OwnerOverlayRead
{
    Missing,
    Usable,
    // Present, but it could not be opened even after a short wait: another reader or a scanner
    // may be holding it.
    Unopenable,
    // Present and readable, but not an overlay any load can use: not JSON, not schema 1, or larger
    // than Settings ever writes one (kMaxOwnerOverlayBytes, kMaxOwnerOverlayEntries).
    Unusable,
};

// Settings writes a few dozen entries, a few kilobytes. Anything far beyond that is not an
// overlay Settings wrote, and reading it in full would cost every load (and the window's thread)
// for nothing.
inline constexpr std::uintmax_t kMaxOwnerOverlayBytes = 64 * 1024;
inline constexpr std::size_t kMaxOwnerOverlayEntries = 256;

// How the warning a load gives for an overlay it could not use begins; the session looks for it
// to tell Settings the whole store is affected.
inline constexpr std::string_view kOwnerOverlayUnusableWarning = "The owner settings file could not be read";

// Reads the overlay the way every load and every write sees it. Only entries a load applies are
// kept, rebuilt as {"schemaVersion":1,"values":{...}}, so nothing else in the file (an unknown
// top-level member, a value of the wrong kind, however deeply nested) is ever copied or dumped.
// Each entry left out adds a warning to outWarnings when it is given, at most a few per read. It
// never reads while a write is swapping the file in, and it retries briefly when the file is
// present but cannot be opened.
OwnerOverlayRead ReadOwnerOverlay(const std::filesystem::path& path, nlohmann::json& outOverlay,
    std::vector<std::string>* outWarnings = nullptr);

// What a load lays over settings.json when the owner's overlay exists but cannot be used: every
// privacy or risky owner setting on its safe side (screen vision, continuous awareness and app
// awareness off, the cloud off and only when asked, the local API untrusted, suggested code not
// built and run, pictures offline, and every tool server in settings asking before each call). A
// damaged file must not quietly bring back something the owner turned off.
[[nodiscard]] nlohmann::json FailClosedOwnerOverlay(const nlohmann::json& settings);

// Lays the owner overlay ({"schemaVersion":1,"values":{...}}) over parsed settings.json before
// configManager reads it. Each allowlisted value of the right type is written to its JSON path,
// creating sections as needed; an MCP key addresses the "servers" entry whose "id" matches.
// Anything else (an unknown key, a wrong type, a missing server, a path blocked by a value
// that is not a section) is skipped with a warning, so a stale overlay cannot stop Revia from
// starting. Returns false, with a warning, when the overlay as a whole is unusable.
bool ApplyOwnerOverrides(nlohmann::json& data, const nlohmann::json& overlay, std::vector<std::string>& warnings);

// The only writer for RuntimeData/Settings/owner-settings.json. Config/settings.json is never
// written: the build and every update copy it from source, so an owner choice kept there would
// be lost. Each change is written to a sibling file and loaded together with settings.json by a
// probe configManager. The probe must show the new value and must not set aside or ignore any
// entry that the stored overlay does not already lose; only then is the file swapped in,
// keeping the previous overlay as owner-settings.json.bak. An overlay that no load can use
// (not JSON, not schema 1, or oversized) is moved aside under a new name before the first change,
// and the new one starts from FailClosedOwnerOverlay, which is what the loads were using.
class SettingsFileEditor
{
public:
    SettingsFileEditor(std::filesystem::path overlayPath, std::string settingsPath, std::string profileDirectory);

    bool SetBool(const std::string& key, bool value, std::string& outError);
    bool SetChoice(const std::string& key, const std::string& value, std::string& outError);
    // Returns the key to whatever settings.json says. Removing a key that is not set succeeds.
    bool Remove(const std::string& key, std::string& outError);
    // The value the overlay holds for the key, or empty when settings.json decides it.
    [[nodiscard]] std::optional<nlohmann::json> Value(const std::string& key) const;
    [[nodiscard]] const std::filesystem::path& Path() const;
    // Set by the last write when it found an overlay no load could use and moved it aside
    // before starting a new one: says where the old file went. Empty otherwise.
    [[nodiscard]] const std::string& Notice() const;

private:
    bool Write(const std::string& key, const std::optional<nlohmann::json>& value, std::string& outError);

    std::filesystem::path overlayPath;
    std::string settingsPath;
    std::string profileDirectory;
    std::string notice;
};

} // namespace revia::core
