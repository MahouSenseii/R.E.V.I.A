#include "Core/settingsFileEditor.h"

#include "Core/appSettings.h"
#include "Core/atomicFile.h"
#include "Core/configManager.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <mutex>
#include <system_error>
#include <utility>

namespace revia::core
{

namespace
{
using json = nlohmann::json;

// Every companion's session writes the same install-wide overlay, each under its own lock.
// This keeps one read-modify-replace from losing another's change.
std::mutex overlayWriteMutex;

constexpr std::string_view McpPrefix = "mcp.servers[";

struct BooleanReader
{
    std::string_view key;
    bool (*read)(const appSettings&);
};

// One reader per non-MCP Bool entry in kOwnerKeys, naming the field configManager fills from
// that JSON path. The editor checks a candidate against it, so a key whose path configManager
// never reads is refused rather than saved and ignored.
constexpr std::array BooleanReaders = {
    BooleanReader{"vision.enabled", [](const appSettings& s) { return s.vision.bEnabled; }},
    BooleanReader{"vision.continuousAwareness", [](const appSettings& s) { return s.vision.bContinuousAwareness; }},
    BooleanReader{"perception.enabled", [](const appSettings& s) { return s.perception.bEnabled; }},
    BooleanReader{"llm.visionEnabled", [](const appSettings& s) { return s.llm.bVisionEnabled; }},
    BooleanReader{"llm.autoTune", [](const appSettings& s) { return s.llm.bAutoTune; }},
    BooleanReader{"llm.autoMaxTokens", [](const appSettings& s) { return s.llm.bAutoMaxTokens; }},
    BooleanReader{"llm.autoStartServer", [](const appSettings& s) { return s.llm.bAutoStartServer; }},
    BooleanReader{"llm.cloud.enabled", [](const appSettings& s) { return s.llm.cloud.bEnabled; }},
    BooleanReader{"embedding.enabled", [](const appSettings& s) { return s.embedding.bEnabled; }},
    BooleanReader{"embedding.autoStartServer", [](const appSettings& s) { return s.embedding.bAutoStartServer; }},
    BooleanReader{"intelligence.enabled", [](const appSettings& s) { return s.intelligence.bEnabled; }},
    BooleanReader{"intelligence.fast.enabled", [](const appSettings& s) { return s.intelligence.fast.bEnabled; }},
    BooleanReader{"intelligence.fast.warmAtStartup", [](const appSettings& s) { return s.intelligence.fast.bWarmAtStartup; }},
    BooleanReader{"intelligence.expert.enabled", [](const appSettings& s) { return s.intelligence.expert.bEnabled; }},
    BooleanReader{"intelligence.expert.onDemand", [](const appSettings& s) { return s.intelligence.expert.bOnDemand; }},
    BooleanReader{"intelligence.expert.warmAtStartup", [](const appSettings& s) { return s.intelligence.expert.bWarmAtStartup; }},
    BooleanReader{"image.enabled", [](const appSettings& s) { return s.image.bEnabled; }},
    BooleanReader{"image.keepLoaded", [](const appSettings& s) { return s.image.bKeepLoaded; }},
    BooleanReader{"image.offline", [](const appSettings& s) { return s.image.bOffline; }},
    BooleanReader{"localApi.enabled", [](const appSettings& s) { return s.localApi.bEnabled; }},
    BooleanReader{"localApi.trustedPrivate", [](const appSettings& s) { return s.localApi.bTrustedPrivate; }},
    BooleanReader{"improvement.enabled", [](const appSettings& s) { return s.improvement.bEnabled; }},
    BooleanReader{"improvement.explore", [](const appSettings& s) { return s.improvement.bExplore; }},
    BooleanReader{"improvement.verify", [](const appSettings& s) { return s.improvement.bVerify; }},
    BooleanReader{"resources.autoPlan", [](const appSettings& s) { return s.resources.bAutoPlan; }},
    BooleanReader{"resources.allowChatModelSplit", [](const appSettings& s) { return s.resources.bAllowChatModelSplit; }},
    BooleanReader{"speechRecognition.useGpu", [](const appSettings& s) { return s.speechRecognition.bUseGpu; }},
};

bool IsServerId(const std::string_view id)
{
    return !id.empty() && id.size() <= 24 &&
        std::all_of(id.begin(), id.end(), [](const char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; });
}

bool IsChoice(const OwnerKey& entry, const std::string& value)
{
    std::string_view rest = entry.choices;
    while (!rest.empty())
    {
        const std::size_t bar = rest.find('|');
        if (rest.substr(0, bar) == value)
            return true;
        rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);
    }
    return false;
}

bool HasOwnerType(const OwnerKey& entry, const json& value)
{
    return entry.type == OwnerKeyType::Bool ? value.is_boolean() : value.is_string() && IsChoice(entry, value.get<std::string>());
}

bool SetPath(json& data, const std::string& key, const std::string& serverId, const json& value, std::string& outReason)
{
    if (!serverId.empty())
    {
        const auto mcp = data.find("mcp");
        if (mcp == data.end() || !mcp->is_object() || !mcp->contains("servers") || !(*mcp)["servers"].is_array())
        {
            outReason = "Config/settings.json lists no MCP servers.";
            return false;
        }
        for (json& server : (*mcp)["servers"])
        {
            if (server.is_object() && server.contains("id") && server["id"].is_string() && server["id"].get<std::string>() == serverId)
            {
                server[key.substr(key.rfind('.') + 1)] = value;
                return true;
            }
        }
        outReason = "Config/settings.json has no MCP server with the id \"" + serverId + "\".";
        return false;
    }

    json* node = &data;
    std::size_t start = 0;
    for (std::size_t dot = key.find('.'); dot != std::string::npos; start = dot + 1, dot = key.find('.', start))
    {
        const std::string section = key.substr(start, dot - start);
        if (!node->contains(section))
            (*node)[section] = json::object();
        else if (!(*node)[section].is_object())
        {
            outReason = "Config/settings.json has \"" + section + "\" as a value rather than a section.";
            return false;
        }
        node = &(*node)[section];
    }
    (*node)[key.substr(start)] = value;
    return true;
}

json EmptyOverlay()
{
    return {{"schemaVersion", 1}, {"values", json::object()}};
}

// The stored overlay, or an empty one when it is missing or unusable. An unusable file is
// ignored by every load too, so replacing it loses nothing that was in effect; the .bak
// written by the replacement keeps it for the owner.
json ReadOverlay(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return EmptyOverlay();
    try
    {
        json overlay;
        stream >> overlay;
        if (overlay.is_object() && overlay.contains("values") && overlay["values"].is_object())
            return overlay;
    }
    catch (const std::exception&)
    {
    }
    return EmptyOverlay();
}

} // namespace

const OwnerKey* FindOwnerKey(const std::string_view key, std::string* outServerId)
{
    if (outServerId)
        outServerId->clear();
    std::string pattern(key);
    if (key.substr(0, McpPrefix.size()) == McpPrefix)
    {
        const std::size_t close = key.find(']', McpPrefix.size());
        if (close == std::string_view::npos || close + 1 >= key.size() || key[close + 1] != '.')
            return nullptr;
        const std::string_view id = key.substr(McpPrefix.size(), close - McpPrefix.size());
        if (!IsServerId(id))
            return nullptr;
        if (outServerId)
            *outServerId = std::string(id);
        pattern = std::string(McpPrefix) + "*" + std::string(key.substr(close));
    }
    const auto found = std::find_if(kOwnerKeys.begin(), kOwnerKeys.end(), [&](const OwnerKey& entry) { return entry.key == pattern; });
    if (found == kOwnerKeys.end())
    {
        if (outServerId)
            outServerId->clear();
        return nullptr;
    }
    return &*found;
}

std::optional<json> OwnerSettingValue(const appSettings& settings, const std::string_view key)
{
    std::string serverId;
    if (!FindOwnerKey(key, &serverId))
        return std::nullopt;
    if (!serverId.empty())
    {
        for (const mcpServerSettings& server : settings.mcp.servers)
        {
            if (server.id == serverId)
                return json(key.ends_with(".enabled") ? server.bEnabled : server.bRequireConfirmation);
        }
        return std::nullopt;
    }
    if (key == "llm.cloud.useFor")
        return json(settings.llm.cloud.useFor);
    for (const BooleanReader& reader : BooleanReaders)
    {
        if (reader.key == key)
            return json(reader.read(settings));
    }
    return std::nullopt;
}

bool ApplyOwnerOverrides(json& data, const json& overlay, std::vector<std::string>& warnings)
{
    if (!data.is_object() || !overlay.is_object() || !overlay.contains("schemaVersion") || !overlay["schemaVersion"].is_number_integer() ||
        overlay["schemaVersion"].get<int>() != 1 || !overlay.contains("values") || !overlay["values"].is_object())
    {
        warnings.push_back("The owner settings file is not in the expected form, so it was ignored.");
        return false;
    }
    for (const auto& [key, value] : overlay["values"].items())
    {
        std::string serverId;
        const OwnerKey* entry = FindOwnerKey(key, &serverId);
        if (!entry)
        {
            warnings.push_back("Owner setting \"" + key + "\" is not one Settings may change, so it was ignored.");
            continue;
        }
        if (!HasOwnerType(*entry, value))
        {
            warnings.push_back("Owner setting \"" + key + "\" holds a value of the wrong kind, so it was ignored.");
            continue;
        }
        std::string reason;
        if (!SetPath(data, key, serverId, value, reason))
            warnings.push_back("Owner setting \"" + key + "\" was ignored: " + reason);
    }
    return true;
}

SettingsFileEditor::SettingsFileEditor(std::filesystem::path overlay, std::string settings, std::string profiles)
    : overlayPath(std::move(overlay)), settingsPath(std::move(settings)), profileDirectory(std::move(profiles))
{
}

const std::filesystem::path& SettingsFileEditor::Path() const
{
    return overlayPath;
}

bool SettingsFileEditor::SetBool(const std::string& key, const bool value, std::string& outError)
{
    const OwnerKey* entry = FindOwnerKey(key);
    if (!entry || entry->type != OwnerKeyType::Bool)
    {
        outError = "'" + key + "' is not an on/off setting that Settings may change.";
        return false;
    }
    return Write(key, json(value), outError);
}

bool SettingsFileEditor::SetChoice(const std::string& key, const std::string& value, std::string& outError)
{
    const OwnerKey* entry = FindOwnerKey(key);
    if (!entry || entry->type != OwnerKeyType::Choice)
    {
        outError = "'" + key + "' is not a choice that Settings may change.";
        return false;
    }
    if (!IsChoice(*entry, value))
    {
        outError = "'" + key + "' does not accept '" + value + "'.";
        return false;
    }
    return Write(key, json(value), outError);
}

bool SettingsFileEditor::Remove(const std::string& key, std::string& outError)
{
    if (!FindOwnerKey(key))
    {
        outError = "'" + key + "' is not a setting that Settings may change.";
        return false;
    }
    return Write(key, std::nullopt, outError);
}

std::optional<json> SettingsFileEditor::Value(const std::string& key) const
{
    const json overlay = ReadOverlay(overlayPath);
    const auto found = overlay["values"].find(key);
    if (found == overlay["values"].end())
        return std::nullopt;
    return *found;
}

bool SettingsFileEditor::Write(const std::string& key, const std::optional<json>& value, std::string& outError)
{
    std::lock_guard writeLock(overlayWriteMutex);
    json overlay = ReadOverlay(overlayPath);
    overlay["schemaVersion"] = 1;
    json& values = overlay["values"];
    if (value)
        values[key] = *value;
    else if (values.erase(key) == 0)
    {
        outError.clear();
        return true;
    }

    std::error_code error;
    std::filesystem::create_directories(overlayPath.parent_path(), error);
    if (error)
    {
        outError = "Could not create " + overlayPath.parent_path().string() + ": " + error.message();
        return false;
    }
    std::filesystem::path pending = overlayPath;
    pending += ".pending";
    {
        std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
        stream << overlay.dump(2) << '\n';
        stream.close();
        if (stream.fail())
        {
            std::filesystem::remove(pending, error);
            outError = "Could not write the pending owner settings file.";
            return false;
        }
    }

    // Validated on the merged result, exactly as startup will read it. A value that loads
    // but never reaches its field means the key's path is wrong, so it is refused too.
    configManager probe(settingsPath, profileDirectory);
    probe.SetOwnerOverlay(pending.string(), true);
    appSettings candidate;
    std::vector<std::string> warnings;
    const bool loaded = probe.LoadSettings(candidate, warnings);
    if (!loaded || (value && OwnerSettingValue(candidate, key) != *value))
    {
        std::filesystem::remove(pending, error);
        outError = !loaded ? "Config/settings.json does not accept this change together with the other owner settings, so nothing was saved."
                           : "The change did not reach the loaded settings, so nothing was saved.";
        const auto reason = std::find_if(warnings.begin(), warnings.end(),
            [&key](const std::string& warning) { return warning.find("\"" + key + "\"") != std::string::npos; });
        if (reason != warnings.end())
            outError += " " + *reason;
        return false;
    }
    return ReplaceFileAtomically(overlayPath, pending, outError);
}

} // namespace revia::core
