#include "Runtime/reviaSession.h"

#include "Core/settingsFileEditor.h"
#include "Workspace/virtualMachineSettings.h"

#include <algorithm>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <optional>
#include <utility>
#include <vector>

// What the desktop's Settings dialog reads and writes beyond the preference commands: the
// owner overlay over settings.json, the Ubuntu workspace switches, the session-only switches,
// and one view of every Boolean with what is saved beside what is running.
namespace revia::runtime
{

namespace
{
// Also the dialog's one notice for the whole store, so its rows do not each repeat it.
constexpr const char* UnreadableSettings =
    "Config/settings.json could not be read, so the settings kept in it cannot be shown or changed until it is fixed.";
constexpr const char* CloudSetupNeeded = "Set llm.cloud.baseUrl and llm.cloud.model in Config/settings.json first.";
constexpr const char* PicturesSetupNeeded = "Set llm.multimodalProjectorPath and llm.mediaPath in Config/settings.json first.";
constexpr const char* FollowsSettingsFile = "the value in Config/settings.json";

std::string Trimmed(const std::string& value)
{
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

bool OwnerFlag(const appSettings& settings, const std::string& key, const bool fallback)
{
    const auto value = core::OwnerSettingValue(settings, key);
    return value && value->is_boolean() ? value->get<bool>() : fallback;
}

// An owner setting that only means something inside another one: the value it goes back to,
// and, for a choice, the one value that is not safe (null when anything but the safe value is not).
struct InnerOwnerSetting
{
    std::string key;
    nlohmann::json safe;
    nlohmann::json risky = nullptr;
};

// The owner settings that only mean something inside another one. A narrower setting must not
// survive the one it sits inside and quietly return, without the question it asks, when that one
// is turned on again (the rule CapabilityEditor keeps for capabilities), so turning the outer one
// off puts these back too. Hard questions go back to "only when I ask", which the cloud's own
// question covers.
std::vector<InnerOwnerSetting> InnerOwnerSettings(const std::string& key)
{
    // Keeping track of the screen needs both seeing it and noticing apps.
    if (key == "vision.enabled" || key == "perception.enabled")
        return {{"vision.continuousAwareness", false}};
    if (key == "localApi.enabled")
        return {{"localApi.trustedPrivate", false}};
    if (key == "llm.cloud.enabled")
        return {{"llm.cloud.useFor", "ask", "hardTurns"}};
    if (key == "improvement.enabled")
        return {{"improvement.verify", false}};
    if (key == "image.enabled")
        return {{"image.offline", true}};
    std::string serverId;
    if (core::FindOwnerKey(key, &serverId) && !serverId.empty() && key.ends_with("].enabled"))
        return {{"mcp.servers[" + serverId + "].requireConfirmation", true}};
    return {};
}
// The owner grants that end as soon as they are turned off, as the microphone does: noticing
// apps, a program speaking to her (or speaking as the owner), and suggested code being reviewed
// (or built and run) must not outlast the switch until a restart. Turning one on still waits for
// the restart that starts it. The running field each one stops, in settings.
bool* GrantStoppedWhenOff(appSettings& settings, const std::string& key)
{
    if (key == "perception.enabled")
        return &settings.perception.bEnabled;
    if (key == "localApi.enabled")
        return &settings.localApi.bEnabled;
    if (key == "localApi.trustedPrivate")
        return &settings.localApi.bTrustedPrivate;
    if (key == "improvement.enabled")
        return &settings.improvement.bEnabled;
    if (key == "improvement.verify")
        return &settings.improvement.bVerify;
    return nullptr;
}

bool StopsWhenTurnedOff(const std::string& key)
{
    // Only the address of a field is taken; nothing reads or writes the probe.
    static appSettings probe;
    return GrantStoppedWhenOff(probe, key) != nullptr;
}
} // namespace

conversationSettings ReviaSession::ConversationSnapshot() const
{
    std::lock_guard snapshotLock(preferenceSnapshotMutex);
    return settings.conversation;
}

visionSettings ReviaSession::VisionSnapshot() const
{
    std::lock_guard snapshotLock(preferenceSnapshotMutex);
    return settings.vision;
}

performanceSettings ReviaSession::PerformanceSnapshot() const
{
    std::lock_guard snapshotLock(preferenceSnapshotMutex);
    return settings.performance;
}

bool ReviaSession::SpeechRecognitionEnabledNow() const
{
    std::lock_guard snapshotLock(preferenceSnapshotMutex);
    return settings.speechRecognition.bEnabled;
}

bool ReviaSession::PerceptionEnabledNow() const
{
    std::lock_guard snapshotLock(preferenceSnapshotMutex);
    return settings.perception.bEnabled;
}

void ReviaSession::PublishVisionStatus()
{
    visionSettings vision = VisionSnapshot();
    // Keeping track of the screen runs only while app awareness does: say so only then.
    vision.bContinuousAwareness = vision.bContinuousAwareness && PerceptionEnabledNow();
    RuntimeEvent visionEvent;
    visionEvent.kind = RuntimeEventKind::ComponentStatus;
    visionEvent.state = state.load();
    visionEvent.component = "Vision";
    visionEvent.phase = !vision.bEnabled ? "Disabled" : llmAvailable ? "Ready" : "Unavailable";
    visionEvent.message = !vision.bEnabled ? "Local screen vision is off."
                          : llmAvailable   ? vision.bContinuousAwareness
                                                 ? "Continuous local multi-monitor awareness is watching with temporary captures."
                                                 : "Local screen vision is ready for approved actions."
                                           : "Vision requires the configured multimodal llama.cpp server.";
    visionEvent.resource = resourcePlan.ChatLabel();
    eventBus.Publish(std::move(visionEvent));
}

std::vector<std::string> ReviaSession::WakeWords() const
{
    if (started.load())
    {
        std::lock_guard snapshotLock(preferenceSnapshotMutex);
        return settings.speechRecognition.wakeWords;
    }
    appSettings persisted;
    return config.LoadSettings(persisted) ? persisted.speechRecognition.wakeWords : speechRecognitionSettings{}.wakeWords;
}

std::map<std::string, BooleanSettingState> ReviaSession::BooleanSettings() const
{
    // Never takes preferenceUpdateMutex: a writer can hold it while a file is validated and
    // replaced, and this is read from the window's thread.
    appSettings persisted;
    const bool loaded = config.LoadSettings(persisted);
    if (loaded)
    {
        preferenceStore.Apply(persisted);
    }
    const bool running = started.load();

    std::vector<std::string> preferenceKeys;
    for (const core::PreferenceKey& key : core::PreferenceStore::Writable())
    {
        if (key.type == core::PreferenceType::Boolean)
            preferenceKeys.push_back(key.name);
    }
    std::vector<std::string> ownerKeys;
    for (const core::OwnerKey& entry : core::kOwnerKeys)
    {
        if (entry.type == core::OwnerKeyType::Bool && !entry.key.starts_with("mcp."))
            ownerKeys.emplace_back(entry.key);
    }
    for (const mcpServerSettings& server : persisted.mcp.servers)
    {
        ownerKeys.push_back("mcp.servers[" + server.id + "].enabled");
        ownerKeys.push_back("mcp.servers[" + server.id + "].requireConfirmation");
    }

    // A value that could not be read shows as off, not as a default dressed up as the owner's.
    std::map<std::string, BooleanSettingState> states;
    for (const std::string& key : preferenceKeys)
    {
        BooleanSettingState& state = states[key];
        state.available = loaded;
        state.value = state.running = loaded && core::BooleanPreference(persisted, key).value_or(false);
        if (!loaded)
            state.unavailableReason = UnreadableSettings;
    }
    for (const std::string& key : ownerKeys)
    {
        BooleanSettingState& state = states[key];
        state.available = loaded;
        state.value = state.running = loaded && OwnerFlag(persisted, key, false);
        if (!loaded)
            state.unavailableReason = UnreadableSettings;
    }

    bool perceptionRunning = false;
    std::optional<bool> machineAtStart;
    if (running)
    {
        std::lock_guard snapshotLock(preferenceSnapshotMutex);
        // Restart-only preferences are never committed, so the running settings hold what
        // was started with.
        for (const std::string& key : preferenceKeys)
            states[key].running = core::BooleanPreference(settings, key).value_or(states[key].running);
        // Partly live preferences are committed for their live reader, but the rest of them still
        // runs with what Start read, so a change waits for a restart.
        for (const auto& [key, atStart] : startupPartlyLivePreferences)
            states[key].running = atStart;
        for (const std::string& key : ownerKeys)
        {
            const core::OwnerKey* entry = core::FindOwnerKey(key);
            if (entry->live && !key.starts_with("llm.cloud."))
            {
                // A server added to settings.json since startup is not running at all.
                states[key].running = OwnerFlag(settings, key, false);
            }
            else if (!entry->live)
            {
                const auto atStart = startupOwnerSettings.find(key);
                if (atStart != startupOwnerSettings.end())
                    states[key].running = atStart->second;
            }
        }
        perceptionRunning = settings.perception.bEnabled;
        machineAtStart = virtualMachineEnabledAtStart;
    }
    if (running && loaded)
    {
        // The cloud service holds the running copy; Settings changes it directly.
        states["llm.cloud.enabled"].running = router.Cloud().Settings().bEnabled;
    }

    // A switch that could only be turned on into a settings file that no longer loads.
    if (loaded && !persisted.llm.cloud.bEnabled &&
        (Trimmed(persisted.llm.cloud.baseUrl).empty() || Trimmed(persisted.llm.cloud.model).empty()))
    {
        states["llm.cloud.enabled"].available = false;
        states["llm.cloud.enabled"].unavailableReason = CloudSetupNeeded;
    }
    if (loaded && !persisted.llm.bVisionEnabled && (persisted.llm.multimodalProjectorPath.empty() || persisted.llm.mediaPath.empty()))
    {
        states["llm.visionEnabled"].available = false;
        states["llm.visionEnabled"].unavailableReason = PicturesSetupNeeded;
    }

    BooleanSettingState& paused = states["perception.paused"];
    paused.value = paused.running = IsPerceptionPaused();
    paused.available = perceptionRunning;
    if (!running)
        paused.unavailableReason = "Available once Revia has started.";
    else if (!perceptionRunning)
        paused.unavailableReason = "App awareness is not running.";

    BooleanSettingState& cloudSession = states["cloud.session"];
    cloudSession.available = true;
    cloudSession.value = cloudSession.running = router.Cloud().SessionEnabled();

    BooleanSettingState& machine = states["virtualMachine.enabled"];
    machine.available = true;
    machine.value = IsVirtualMachineEnabled();
    machine.running = machineAtStart.value_or(machine.value);
    return states;
}

std::string ReviaSession::SettingsNotice() const
{
    appSettings persisted;
    std::vector<std::string> warnings;
    if (!config.LoadSettings(persisted, warnings))
        return UnreadableSettings;
    for (const std::string& warning : warnings)
    {
        if (warning.starts_with(core::kOwnerOverlayUnusableWarning))
            return warning;
    }
    return {};
}

core::PreferenceResult ReviaSession::SetOwnerSetting(const std::string& key, const bool value)
{
    const core::OwnerKey* entry = core::FindOwnerKey(key);
    if (!entry || entry->type != core::OwnerKeyType::Bool)
    {
        return {false, "'" + key + "' is not an on/off setting that Settings may change."};
    }
    if (key == "llm.cloud.enabled" && value)
    {
        appSettings persisted;
        if (config.LoadSettings(persisted) &&
            (Trimmed(persisted.llm.cloud.baseUrl).empty() || Trimmed(persisted.llm.cloud.model).empty()))
        {
            return {false, CloudSetupNeeded};
        }
    }
    core::PreferenceResult result = WriteOwnerSetting(
        key, [&](core::SettingsFileEditor& editor, std::string& error) { return editor.SetBool(key, value, error); }, value ? "on" : "off");
    if (result.succeeded)
        SecureInnerOwnerSettings(key, result);
    return result;
}

core::PreferenceResult ReviaSession::SetOwnerChoice(const std::string& key, const std::string& value)
{
    const core::OwnerKey* entry = core::FindOwnerKey(key);
    if (!entry || entry->type != core::OwnerKeyType::Choice)
    {
        return {false, "'" + key + "' is not a choice that Settings may change."};
    }
    return WriteOwnerSetting(
        key, [&](core::SettingsFileEditor& editor, std::string& error) { return editor.SetChoice(key, value, error); }, "\"" + value + "\"");
}

core::PreferenceResult ReviaSession::ResetOwnerSetting(const std::string& key)
{
    if (!core::FindOwnerKey(key))
    {
        return {false, "'" + key + "' is not a setting that Settings may change."};
    }
    core::PreferenceResult result = WriteOwnerSetting(
        key, [&](core::SettingsFileEditor& editor, std::string& error) { return editor.Remove(key, error); }, FollowsSettingsFile);
    if (result.succeeded)
        SecureInnerOwnerSettings(key, result);
    return result;
}

void ReviaSession::SecureInnerOwnerSettings(const std::string& key, core::PreferenceResult& result)
{
    const std::vector<InnerOwnerSetting> inner = InnerOwnerSettings(key);
    appSettings persisted;
    if (inner.empty() || !config.LoadSettings(persisted) || OwnerFlag(persisted, key, true))
        return;
    for (const InnerOwnerSetting& setting : inner)
    {
        const std::optional<nlohmann::json> current = core::OwnerSettingValue(persisted, setting.key);
        if (!current || *current == setting.safe || (!setting.risky.is_null() && *current != setting.risky))
            continue;
        const bool choice = setting.safe.is_string();
        const std::string shown = choice ? "\"" + setting.safe.get<std::string>() + "\"" : (setting.safe.get<bool>() ? "on" : "off");
        // Each goes through the ordinary write, so a live one also changes what is running.
        const core::PreferenceResult secured = WriteOwnerSetting(
            setting.key,
            [&](core::SettingsFileEditor& editor, std::string& error)
            {
                return choice ? editor.SetChoice(setting.key, setting.safe.get<std::string>(), error)
                              : editor.SetBool(setting.key, setting.safe.get<bool>(), error);
            },
            shown);
        result.message += secured.succeeded ? " " + setting.key + " went back to " + shown + " with it."
                                            : " " + setting.key + " could not go back to " + shown + ": " + secured.message;
    }
}

core::PreferenceResult ReviaSession::WriteOwnerSetting(const std::string& key,
    const std::function<bool(core::SettingsFileEditor&, std::string&)>& write, const std::string& description)
{
    std::string serverId;
    const core::OwnerKey* entry = core::FindOwnerKey(key, &serverId);
    bool live = false;
    bool stopsNow = false;
    std::string notice;
    {
        std::lock_guard preferenceLock(preferenceUpdateMutex);
        appSettings persisted;
        if (!config.LoadSettings(persisted))
        {
            return {false, "Config/settings.json could not be read, so nothing was changed."};
        }
        if (!serverId.empty() && !core::OwnerSettingValue(persisted, key))
        {
            return {false, "Config/settings.json has no MCP server with the id \"" + serverId + "\"."};
        }
        core::SettingsFileEditor editor(companionPaths.InstallRoot() / "RuntimeData/Settings/owner-settings.json",
            (companionPaths.InstallRoot() / "Config/settings.json").string(), companionPaths.Resolve("Config/Profiles").string());
        std::string error;
        const bool written = write(editor, error);
        if (!editor.Notice().empty())
        {
            appLogger.Warning(editor.Notice());
            notice = " " + editor.Notice();
        }
        if (!written)
        {
            return {false, error + notice};
        }
        appLogger.Log("Owner changed " + key + " to " + description + " from Settings.");
        // A live key also changes the running session, from the value just saved as the
        // merged settings now read it, and so does a grant that stops when turned off. Until
        // Start sets started, under this lock, it commits these itself, so a change saved
        // before then is not lost.
        if ((entry->live || StopsWhenTurnedOff(key)) && started.load() && config.LoadSettings(persisted))
        {
            stopsNow = !entry->live && !OwnerFlag(persisted, key, true);
            if (entry->live || stopsNow)
                live = CommitOwnerSettingLocked(key, persisted);
        }
    }
    if (live)
    {
        RunLiveApplier(key, true);
    }
    PublishComponent("Preferences", "Changed", key);
    const std::string change = description == FollowsSettingsFile ? " follows Config/settings.json again" : " is now " + description;
    const std::string when = entry->live ? std::string()
                             : stopsNow  ? std::string(" It stops now; turning it back on takes effect the next time Revia starts.")
                                         : std::string(" It takes effect the next time Revia starts.");
    return {true, key + change + ", for every companion." + when + notice};
}

bool ReviaSession::CommitOwnerSettingLocked(const std::string& key, const appSettings& persisted)
{
    if (key.starts_with("llm.cloud."))
    {
        // Every turn asks the cloud service, so its copy is the running value, and replacing
        // it is only an assignment. settings.llm stays as started: model startup copies it
        // whole without this lock. This is what keeps /cloud on from undoing Settings.
        llm::cloud::CloudModelSettings cloud = router.Cloud().Settings();
        cloud.bEnabled = persisted.llm.cloud.bEnabled;
        cloud.useFor = persisted.llm.cloud.useFor;
        router.Cloud().ApplySettings(cloud);
        return false;
    }
    std::lock_guard snapshotLock(preferenceSnapshotMutex);
    if (bool* running = GrantStoppedWhenOff(settings, key))
    {
        // Reached only when it was turned off. What is running is now off, so Settings counts a
        // later "on" as waiting for a restart.
        const bool wasRunning = *running;
        *running = false;
        startupOwnerSettings[key] = false;
        return wasRunning;
    }
    if (key == "vision.enabled")
    {
        settings.vision.bEnabled = persisted.vision.bEnabled;
        return true;
    }
    if (key == "vision.continuousAwareness")
    {
        settings.vision.bContinuousAwareness = persisted.vision.bContinuousAwareness;
        return true;
    }
    std::string serverId;
    if (!core::FindOwnerKey(key, &serverId) || serverId.empty())
    {
        return false;
    }
    const auto stored = std::find_if(persisted.mcp.servers.begin(), persisted.mcp.servers.end(),
        [&serverId](const mcpServerSettings& server) { return server.id == serverId; });
    const auto running = std::find_if(settings.mcp.servers.begin(), settings.mcp.servers.end(),
        [&serverId](const mcpServerSettings& server) { return server.id == serverId; });
    if (stored == persisted.mcp.servers.end() || running == settings.mcp.servers.end())
    {
        // A server added since startup has nothing running to change.
        return false;
    }
    if (key.ends_with(".enabled"))
        running->bEnabled = stored->bEnabled;
    else
        running->bRequireConfirmation = stored->bRequireConfirmation;
    return true;
}

bool ReviaSession::CommitLiveOwnerSettingsLocked()
{
    appSettings persisted;
    if (!config.LoadSettings(persisted))
    {
        return false;
    }
    // The cloud service was configured from the settings Start loaded; see
    // CommitOwnerSettingLocked for why its copy is the one that changes.
    llm::cloud::CloudModelSettings cloud = router.Cloud().Settings();
    if (cloud.bEnabled != persisted.llm.cloud.bEnabled || cloud.useFor != persisted.llm.cloud.useFor)
    {
        cloud.bEnabled = persisted.llm.cloud.bEnabled;
        cloud.useFor = persisted.llm.cloud.useFor;
        router.Cloud().ApplySettings(cloud);
    }
    std::lock_guard snapshotLock(preferenceSnapshotMutex);
    settings.vision.bEnabled = persisted.vision.bEnabled;
    settings.vision.bContinuousAwareness = persisted.vision.bContinuousAwareness;
    // A grant turned off while Start ran stops before anything uses it: its applier runs once
    // Start drains them, and what Start launches after this reads it as off.
    for (const core::OwnerKey& entry : core::kOwnerKeys)
    {
        const std::string key(entry.key);
        bool* running = GrantStoppedWhenOff(settings, key);
        if (running == nullptr || !*running || OwnerFlag(persisted, key, true))
            continue;
        *running = false;
        startupOwnerSettings[key] = false;
        std::lock_guard dueLock(dueAppliersMutex);
        dueAppliers.push_back({key, true});
    }
    bool mcpChanged = false;
    for (mcpServerSettings& running : settings.mcp.servers)
    {
        const auto stored = std::find_if(persisted.mcp.servers.begin(), persisted.mcp.servers.end(),
            [&running](const mcpServerSettings& server) { return server.id == running.id; });
        if (stored != persisted.mcp.servers.end() &&
            (running.bEnabled != stored->bEnabled || running.bRequireConfirmation != stored->bRequireConfirmation))
        {
            running.bEnabled = stored->bEnabled;
            running.bRequireConfirmation = stored->bRequireConfirmation;
            mcpChanged = true;
        }
    }
    return mcpChanged;
}

void ReviaSession::ApplyOwnerSetting(const std::string& key)
{
    // Before Start has launched what these change, or once Stop has begun taking it down,
    // there is nothing to change: Start reads the committed value itself.
    if (!liveWorkersOpen)
    {
        return;
    }
    if (key.starts_with("mcp."))
    {
        mcpSettings mcp;
        {
            std::lock_guard snapshotLock(preferenceSnapshotMutex);
            mcp = settings.mcp;
        }
        // Diffs by server: a changed server is replaced and its connection closed, the rest
        // keep theirs.
        mcpManager.Configure(mcp);
        return;
    }
    if (key == "perception.enabled")
    {
        // Only turning it off is committed; turning it on waits for the hook Start installs.
        if (PerceptionEnabledNow())
            return;
        windowEventMonitor.Shutdown();
        ClearEditorObservation();
        conversationStarter.Clear();
        screenAwareness.Clear();
        // Keeping track of the screen runs only while app awareness does.
        StopScreenAwareness();
        PublishComponent("Perception", "Off",
            "App awareness is off. Turning Notice which app I'm using back on takes effect the next time Revia starts.");
        PublishVisionStatus();
        return;
    }
    if (key.starts_with("localApi."))
    {
        ClearEditorObservation();
        // Off, or no longer trusted: the running API stops, and only an API that is still on
        // listens again, as a shared audience. One that was not running stays stopped.
        if (const auto api = localApiRuntime.exchange({}))
        {
            api->Stop();
            StartLocalApi();
            if (!localApiRuntime.load())
                PublishComponent("Local API", "Disabled", "The local API is off. Turning it back on takes effect the next time Revia starts.");
        }
        return;
    }
    if (key.starts_with("improvement."))
    {
        // Off, or no longer building what it suggests: the reviewer stops (and a build with it),
        // and one that is still on starts again without a workbench.
        if (improvementAgent.Running())
        {
            StopSelfImprovement();
            StartSelfImprovement();
        }
        return;
    }
    if (key == "vision.enabled" || key == "vision.continuousAwareness")
    {
        PublishVisionStatus();
    }
    // Stops the worker, dropping what it last saw, when vision or continuous awareness is
    // now off, and starts it when both are on and app awareness is running.
    StartScreenAwareness();
}

void ReviaSession::RunLiveApplier(const std::string& key, const bool owner)
{
    {
        std::lock_guard dueLock(dueAppliersMutex);
        const bool due = std::any_of(dueAppliers.begin(), dueAppliers.end(),
            [&](const DueApplier& applier) { return applier.key == key && applier.owner == owner; });
        if (!due)
            dueAppliers.push_back({key, owner});
    }
    DrainLiveAppliers();
}

void ReviaSession::DrainLiveAppliers()
{
    for (;;)
    {
        if (drainingAppliers.exchange(true))
        {
            // The thread draining runs this key too: it looks again once it is done.
            return;
        }
        try
        {
            // Waits only for Start or Stop, which never wait on an applier.
            std::lock_guard applyLock(preferenceApplyMutex);
            for (;;)
            {
                DueApplier next;
                {
                    std::lock_guard dueLock(dueAppliersMutex);
                    if (dueAppliers.empty())
                        break;
                    next = std::move(dueAppliers.front());
                    dueAppliers.erase(dueAppliers.begin());
                }
                if (next.owner)
                    ApplyOwnerSetting(next.key);
                else
                    ApplyPreference(next.key);
            }
        }
        catch (...)
        {
            drainingAppliers.store(false);
            throw;
        }
        drainingAppliers.store(false);
        // A caller that found the flag set after the last look left its key here.
        std::lock_guard dueLock(dueAppliersMutex);
        if (dueAppliers.empty())
            return;
    }
}

std::string ReviaSession::OwnerChoice(const std::string& key) const
{
    appSettings persisted;
    if (!config.LoadSettings(persisted))
        return {};
    const auto value = core::OwnerSettingValue(persisted, key);
    return value && value->is_string() ? value->get<std::string>() : std::string{};
}

std::vector<std::string> ReviaSession::McpServerNames() const
{
    appSettings persisted;
    std::vector<std::string> names;
    if (config.LoadSettings(persisted))
    {
        for (const mcpServerSettings& server : persisted.mcp.servers)
            names.push_back(server.id);
    }
    return names;
}

CapabilityUpdateResult ReviaSession::SetGuestCapabilities(const policy::GuestCapabilitySettings& guest)
{
    CapabilityUpdateResult result;
    {
        std::lock_guard preferenceLock(preferenceUpdateMutex);
        std::string error;
        result.succeeded = actionRuntime.SetGuestCapabilities(guest, error);
        result.message = result.succeeded ? guest.enabled ? "Ubuntu workspace permissions saved." : "Actions in the Ubuntu workspace are off."
                                          : error;
    }
    PublishComponent("Permissions", result.succeeded ? "Saved" : "Error", result.message);
    return result;
}

core::PreferenceResult ReviaSession::SetVirtualMachineEnabled(const bool enabled)
{
    {
        std::lock_guard preferenceLock(preferenceUpdateMutex);
        std::string error;
        if (!workspace::SaveVirtualMachineSettings(
                companionPaths.Resolve("RuntimeData/VirtualMachine/settings.json"), {enabled, companionPaths.Descriptor().id}, error))
        {
            return {false, error};
        }
        appLogger.Log(std::string("Owner turned Ubuntu workspace startup ") + (enabled ? "on" : "off") + " from Settings.");
    }
    PublishComponent("Preferences", "Changed", "virtualMachine.enabled");
    return {true, enabled ? "The Ubuntu workspace will start the next time Revia starts."
                          : "The Ubuntu workspace will not start the next time Revia starts."};
}

bool ReviaSession::IsVirtualMachineEnabled() const
{
    const auto configured = workspace::LoadVirtualMachineSettings(
        companionPaths.Resolve("RuntimeData/VirtualMachine/settings.json"), companionPaths.Descriptor().id);
    return configured.succeeded && configured.settings.enabled;
}

void ReviaSession::SetCloudSessionEnabled(const bool enabled)
{
    router.Cloud().SetSessionEnabled(enabled);
    PublishComponent("Preferences", "Changed", "cloud.session");
}

bool ReviaSession::IsCloudSessionEnabled() const
{
    return router.Cloud().SessionEnabled();
}

std::string ReviaSession::CloudStatusText() const
{
    std::string text = router.Cloud().StatusText();
    const std::string variable = Trimmed(router.Cloud().Settings().apiKeyEnvironmentVariable);
    if (!variable.empty())
    {
        // Only whether it is set. The key itself never leaves the cloud service.
        const char* key = std::getenv(variable.c_str());
        text += key != nullptr && *key != '\0' ? " API key found in " + variable + "." : " API key missing: set " + variable + ".";
    }
    return text;
}

std::optional<ProfileSummary> ReviaSession::ActiveProfileSummary() const
{
    if (!started.load())
        return std::nullopt;
    std::string profileId;
    {
        std::lock_guard snapshotLock(preferenceSnapshotMutex);
        profileId = settings.activeProfile;
    }
    aiProfile loaded;
    if (!config.LoadProfile(profileId, loaded))
        return std::nullopt;
    ProfileSummary summary;
    summary.id = profileId;
    summary.displayName = loaded.displayName;
    summary.description = loaded.description;
    summary.systemPrompt = loaded.systemPrompt;
    summary.memoryEnabled = loaded.bMemoryEnabled;
    summary.hasTemperatureOverride = loaded.bHasTemperatureOverride;
    summary.temperature = loaded.temperature;
    summary.hasMaxTokensOverride = loaded.bHasMaxTokensOverride;
    summary.maxTokens = loaded.maxTokens;
    summary.answerObligation = loaded.answerObligation;
    return summary;
}

} // namespace revia::runtime
