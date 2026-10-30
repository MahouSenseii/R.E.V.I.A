#include "Core/settingsFileEditor.h"

#include "Core/appSettings.h"
#include "Core/atomicFile.h"
#include "Core/configManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <utility>

namespace revia::core
{

namespace
{
using json = nlohmann::json;

// Every companion's session writes the same install-wide overlay, each under its own lock.
// This keeps one read-modify-replace from losing another's change.
std::mutex overlayWriteMutex;
// Held by every read of the overlay and by the swap that replaces it, and only for those: a load
// never sees the moment the old file has gone and the new one is not yet in place (which would
// read as "no owner choices"), and never waits for a write's validation.
std::mutex overlaySwapMutex;
// A read that finds a file it cannot open tries again for about this long.
constexpr int OpenAttempts = 5;
constexpr std::chrono::milliseconds OpenRetryDelay{20};
// At most this many left-out entries are named per read; the rest are counted in one line.
constexpr std::size_t NamedWarnings = 8;

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
    BooleanReader{"video.enabled", [](const appSettings& s) { return s.video.bEnabled; }},
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
    (*node)[key.substr(start)] = key == "llm.imageMinTokens" ? json(std::stoi(value.get<std::string>())) : value;
    return true;
}

json EmptyOverlay()
{
    return {{"schemaVersion", 1}, {"values", json::object()}};
}

// The form ApplyOwnerOverrides accepts. Anything else is ignored by every load.
bool IsUsableOverlay(const json& overlay)
{
    return overlay.is_object() && overlay.contains("schemaVersion") && overlay["schemaVersion"].is_number_integer() &&
           overlay["schemaVersion"].get<int>() == 1 && overlay.contains("values") && overlay["values"].is_object();
}

// A name beside the overlay that nothing uses yet, for keeping an unusable one.
std::filesystem::path UnusedAsideName(const std::filesystem::path& path)
{
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::filesystem::path aside;
    for (int suffix = 0; suffix < 100; ++suffix)
    {
        aside = path;
        aside += ".unreadable-" + std::to_string(stamp) + (suffix == 0 ? std::string() : "-" + std::to_string(suffix));
        std::error_code error;
        if (!std::filesystem::exists(aside, error))
            break;
    }
    return aside;
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

bool IsAcceptedOwnerValue(const std::string_view key, const json& value)
{
    const OwnerKey* entry = FindOwnerKey(key);
    return entry != nullptr && HasOwnerType(*entry, value);
}

OwnerOverlayRead ReadOwnerOverlay(const std::filesystem::path& path, json& outOverlay, std::vector<std::string>* outWarnings)
{
    outOverlay = EmptyOverlay();
    std::string text;
    for (int attempt = 0;; ++attempt)
    {
        {
            std::lock_guard swapLock(overlaySwapMutex);
            std::error_code error;
            const bool present = std::filesystem::exists(path, error);
            if (!present && !error)
                return OwnerOverlayRead::Missing;
            std::ifstream stream(path, std::ios::binary);
            if (stream)
            {
                // At most one byte past the limit is read, whatever size the file claims.
                text.assign(static_cast<std::size_t>(kMaxOwnerOverlayBytes) + 1, '\0');
                stream.read(text.data(), static_cast<std::streamsize>(text.size()));
                text.resize(static_cast<std::size_t>(stream.gcount()));
                if (!stream.bad())
                    break;
            }
        }
        if (attempt + 1 >= OpenAttempts)
            return OwnerOverlayRead::Unopenable;
        std::this_thread::sleep_for(OpenRetryDelay);
    }
    if (text.size() > kMaxOwnerOverlayBytes)
        return OwnerOverlayRead::Unusable;
    json overlay;
    try
    {
        overlay = json::parse(text);
    }
    catch (const std::exception&)
    {
        return OwnerOverlayRead::Unusable;
    }
    if (!IsUsableOverlay(overlay) || overlay["values"].size() > kMaxOwnerOverlayEntries)
        return OwnerOverlayRead::Unusable;
    // Only what a load applies is kept, and only it is copied: a boolean or a choice. A write
    // therefore never carries (or recursively dumps) anything hand-written beside it.
    std::size_t leftOut = 0;
    for (const auto& [key, value] : overlay["values"].items())
    {
        if (IsAcceptedOwnerValue(key, value))
        {
            outOverlay["values"][key] = value;
            continue;
        }
        if (outWarnings != nullptr && leftOut++ < NamedWarnings)
        {
            outWarnings->push_back(FindOwnerKey(key) == nullptr
                                       ? "Owner setting \"" + key + "\" is not one Settings may change, so it was ignored."
                                       : "Owner setting \"" + key + "\" holds a value of the wrong kind, so it was ignored.");
        }
    }
    if (outWarnings != nullptr && leftOut > NamedWarnings)
        outWarnings->push_back(std::to_string(leftOut - NamedWarnings) + " more owner settings could not be used, so they were ignored.");
    return OwnerOverlayRead::Usable;
}

json FailClosedOwnerOverlay(const json& settings)
{
    json values = {{"vision.enabled", false}, {"vision.continuousAwareness", false}, {"perception.enabled", false},
        {"llm.cloud.enabled", false}, {"llm.cloud.useFor", "ask"}, {"localApi.trustedPrivate", false}, {"improvement.verify", false},
        {"image.offline", true}};
    const auto mcp = settings.find("mcp");
    if (settings.is_object() && mcp != settings.end() && mcp->is_object())
    {
        const auto servers = mcp->find("servers");
        if (servers != mcp->end() && servers->is_array())
        {
            for (const json& server : *servers)
            {
                const auto id = server.is_object() ? server.find("id") : server.end();
                if (server.is_object() && id != server.end() && id->is_string() && IsServerId(id->get<std::string>()))
                    values[std::string(McpPrefix) + id->get<std::string>() + "].requireConfirmation"] = true;
            }
        }
    }
    return {{"schemaVersion", 1}, {"values", std::move(values)}};
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
    if (key == "llm.imageMinTokens")
        return json(std::to_string(settings.llm.imageMinTokens));
    for (const BooleanReader& reader : BooleanReaders)
    {
        if (reader.key == key)
            return json(reader.read(settings));
    }
    return std::nullopt;
}

bool ApplyOwnerOverrides(json& data, const json& overlay, std::vector<std::string>& warnings)
{
    if (!data.is_object() || !IsUsableOverlay(overlay))
    {
        warnings.push_back("The owner settings file is not in the expected form, so it was ignored.");
        return false;
    }
    // Each load logs and publishes its warnings, so a file with many bad entries names only a few.
    std::size_t ignored = 0;
    const auto warn = [&](std::string warning)
    {
        if (ignored++ < NamedWarnings)
            warnings.push_back(std::move(warning));
    };
    for (const auto& [key, value] : overlay["values"].items())
    {
        std::string serverId;
        const OwnerKey* entry = FindOwnerKey(key, &serverId);
        if (!entry)
        {
            warn("Owner setting \"" + key + "\" is not one Settings may change, so it was ignored.");
            continue;
        }
        if (!HasOwnerType(*entry, value))
        {
            warn("Owner setting \"" + key + "\" holds a value of the wrong kind, so it was ignored.");
            continue;
        }
        std::string reason;
        if (!SetPath(data, key, serverId, value, reason))
            warn("Owner setting \"" + key + "\" was ignored: " + reason);
    }
    if (ignored > NamedWarnings)
        warnings.push_back(std::to_string(ignored - NamedWarnings) + " more owner settings could not be used, so they were ignored.");
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
    // What the overlay itself holds; one that is missing or unusable holds nothing.
    json overlay;
    static_cast<void>(ReadOwnerOverlay(overlayPath, overlay));
    const auto found = overlay["values"].find(key);
    if (found == overlay["values"].end())
        return std::nullopt;
    return *found;
}

const std::string& SettingsFileEditor::Notice() const
{
    return notice;
}

bool SettingsFileEditor::Write(const std::string& key, const std::optional<json>& value, std::string& outError)
{
    std::lock_guard writeLock(overlayWriteMutex);
    notice.clear();
    json overlay;
    switch (ReadOwnerOverlay(overlayPath, overlay))
    {
    case OwnerOverlayRead::Missing:
    case OwnerOverlayRead::Usable:
        break;
    case OwnerOverlayRead::Unopenable:
        outError = overlayPath.string() + " could not be opened just now, so nothing was saved. Try again in a moment.";
        return false;
    case OwnerOverlayRead::Unusable:
    {
        // Every load already uses the fail-closed overlay in its place, so the new file starts
        // from it: the change takes away nothing in effect and brings back nothing turned off.
        // The old file is kept under its own name rather than overwritten, because the owner
        // may want what is in it.
        const std::filesystem::path aside = UnusedAsideName(overlayPath);
        std::error_code moveError;
        std::filesystem::rename(overlayPath, aside, moveError);
        if (moveError)
        {
            outError = overlayPath.string() + " is not a file Settings can read, and it could not be moved aside (" + moveError.message() +
                       "), so nothing was saved.";
            return false;
        }
        json settings = json::object();
        try
        {
            std::ifstream settingsStream(settingsPath, std::ios::binary);
            settings = json::parse(settingsStream);
        }
        catch (const std::exception&)
        {
            // No tool servers to name; the rest of the safe side needs nothing from the file.
        }
        overlay = FailClosedOwnerOverlay(settings);
        notice = overlayPath.filename().string() + " could not be read, so it was moved to " + aside.filename().string() +
                 ". Settings started a new one with screen vision, app awareness, the cloud model, the trusted local API and "
                 "building suggested code off, as Revia was already running them.";
        break;
    }
    }
    if (value)
        overlay["values"][key] = *value;
    else if (overlay["values"].erase(key) == 0)
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

    // Validated on the merged result, loaded exactly as startup will load it. A stored entry
    // that settings.json no longer accepts is set aside there rather than stopping the start,
    // so it must not stop an unrelated change either: only a change that makes the load set
    // aside or ignore something it did not before is refused. A value that loads but never
    // reaches its field means the key's path is wrong, so it is refused too.
    std::vector<std::string> before;
    {
        configManager current(settingsPath, profileDirectory);
        current.SetOwnerOverlay(overlayPath.string());
        appSettings ignored;
        static_cast<void>(current.LoadSettings(ignored, before));
    }
    configManager probe(settingsPath, profileDirectory);
    probe.SetOwnerOverlay(pending.string());
    appSettings candidate;
    std::vector<std::string> after;
    const bool loaded = probe.LoadSettings(candidate, after);
    std::vector<std::string> added;
    const std::set<std::string> known(before.begin(), before.end());
    for (const std::string& warning : after)
    {
        if (!known.contains(warning))
            added.push_back(warning);
    }
    const bool reached = !value || OwnerSettingValue(candidate, key) == *value;
    if (!loaded || !added.empty() || !reached)
    {
        std::filesystem::remove(pending, error);
        if (!loaded)
            outError = "Config/settings.json could not be read, so nothing was saved.";
        else if (!reached)
            outError = "The change did not reach the loaded settings, so nothing was saved.";
        else
            outError = "Config/settings.json does not accept this change together with the other owner settings, so nothing was saved.";
        // Names why its own value never arrived, and whatever else the change would set aside.
        for (const std::string& warning : after)
        {
            const bool own = warning.find("\"" + key + "\"") != std::string::npos;
            if ((own && !reached) || std::find(added.begin(), added.end(), warning) != added.end())
                outError += " " + warning;
        }
        return false;
    }
    std::lock_guard swapLock(overlaySwapMutex);
    return ReplaceFileAtomically(overlayPath, pending, outError);
}

} // namespace revia::core
