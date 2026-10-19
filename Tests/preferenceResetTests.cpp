#include "reviaSessionTestAccess.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace
{
using revia::core::PreferenceStore;
using revia::runtime::ReviaSession;
using Access = revia::runtime::ReviaSessionTestAccess;
using revia::tests::Check;
using json = nlohmann::json;
using namespace std::chrono_literals;

class WorkingDirectory
{
public:
    explicit WorkingDirectory(const std::filesystem::path& path)
        : previous(std::filesystem::current_path()) { std::filesystem::current_path(path); }
    ~WorkingDirectory() { std::filesystem::current_path(previous); }
private:
    std::filesystem::path previous;
};

void Write(const std::filesystem::path& path, const json& value)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << value.dump(2);
    output.close();
    Check(!output.fail(), "Could not write preference reset fixture.");
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

// patch is merged over the fixture settings.
void Configure(const std::filesystem::path& root, const json& patch = json::object())
{
    json settings = {
        {"activeProfile", "fixture"},
        {"llm", {{"backend", "None"}, {"autoStartServer", false}, {"visionEnabled", false},
            {"modelPath", (root / "absent.gguf").string()},
            {"mediaPath", (root / "RuntimeData/Vision").string()}}},
        {"intelligence", {{"enabled", false}}},
        {"embedding", {{"enabled", false}, {"autoStartServer", false}}},
        {"speech", {{"enabled", true}, {"backend", "WindowsSapi"}, {"speakGreeting", false},
            {"voiceDataPath", (root / "RuntimeData/Voices").string()}}},
        {"speechRecognition", {{"enabled", false}}},
        {"presence", {{"enabled", false}, {"externalAdaptersEnabled", false}}},
        {"vision", {{"enabled", false}}}, {"perception", {{"enabled", false}}},
        {"initiative", {{"enabled", true}, {"curiosityEnabled", true}, {"spontaneousSpeechEnabled", false}}},
        {"bargeIn", {{"enabled", false}}}, {"conversation", {{"archiveEnabled", false}}},
        {"image", {{"enabled", false}}}, {"resources", {{"startupSampleSeconds", 0}}}
    };
    settings.merge_patch(patch);
    Write(root / "Config/settings.json", settings);
    Write(root / "Config/Profiles/fixture.json", {
        {"id", "fixture"}, {"systemPrompt", "Preference reset fixture."}, {"memoryEnabled", false}
    });
}

void LiveUnsetRestoresOnlyTheConfiguredKey()
{
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Configure(directory.root);
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    Check(session.Start(), "Preference reset session did not start.");
    Access::PlannedMainDevice(session, "fixture-planned-device");
    Check(session.SetPreference("speech.enabled", "off").succeeded, "Could not set speech override.");
    Check(!session.UserPreferences().speechEnabled, "Speech override did not apply live.");
    Check(session.SetPreference("initiative.enabled", "off").succeeded, "Could not set initiative override.");
    Check(!session.UserPreferences().initiativeEnabled && !Access::InitiativeWorkersRunning(session),
        "Disabled initiative left its workers running.");
    const auto result = session.Submit("/unset speech.enabled");
    Check(result.succeeded, "Speech unset failed: " + result.reason);
    Check(session.UserPreferences().speechEnabled, "Unset removed speech override but did not restore the configured live value.");
    Check(!session.UserPreferences().initiativeEnabled, "Unsetting speech changed a separate override.");
    Check(Access::PlannedMainDevice(session) == "fixture-planned-device", "Unset overwrote a planned model device.");
    revia::core::PreferenceStore preferences(directory.root / "preferences.json");
    Check(!preferences.Load().contains("speech.enabled") && preferences.Load().contains("initiative.enabled"),
        "Unset did not remove exactly its persisted key.");
    Check(session.Submit("/unset initiative.enabled").succeeded, "Initiative unset failed.");
    Check(session.UserPreferences().initiativeEnabled && Access::InitiativeWorkersRunning(session),
        "Unset did not restore the configured initiative workers.");
    Check(session.SetPreference("activeProfile", "next-start-only").succeeded, "Could not set next-start profile.");
    Check(session.Submit("/unset activeProfile").succeeded, "Next-start profile unset failed.");
    Check(session.ProfileStudio().activeProfileId == "fixture", "Unset changed the running profile.");
    Check(!preferences.Load().contains("activeProfile"), "Unset retained the next-start profile override.");
}

void FailedClearPreservesLiveAndDurableOverride()
{
#ifdef _WIN32
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Configure(directory.root);
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    Check(session.Start(), "Failed-clear session did not start.");
    Check(session.SetPreference("speech.enabled", "off").succeeded, "Could not seed the failed-clear override.");
    const auto path = directory.root / "preferences.json";
    const HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "Could not protect preference fixture against replacement.");
    const auto result = session.Submit("/unset speech.enabled");
    CloseHandle(locked);
    Check(!result.succeeded, "Unset reported success despite a failed durable replacement.");
    Check(!session.UserPreferences().speechEnabled, "Failed clear changed the live preference.");
    revia::core::PreferenceStore preferences(path);
    Check(preferences.Load().at("speech.enabled") == "false", "Failed clear damaged the saved override.");
#endif
}

void ConcurrentPreferenceUpdatesKeepBothLiveValues()
{
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    std::promise<void> entered, release;
    const auto proceed = release.get_future().share();
    const auto subscription = session.Events().Subscribe([&](const auto& event)
    {
        if (event.component != "Response filters") return;
        // A synchronous UI observer may read the current preference view.
        static_cast<void>(session.UserPreferences());
        entered.set_value();
        proceed.wait();
    });
    auto first = std::async(std::launch::async, [&]
    { return session.SetPreference("responseFilter.aiReviewEnabled", "off"); });
    const bool published = entered.get_future().wait_for(2s) == std::future_status::ready;
    auto second = std::async(std::launch::async, [&]
    { return session.SetPreference("speech.enabled", "off"); });
    // Before serialization this finishes while the first update still holds its old
    // settings snapshot. With serialization it waits, then overlays the fresh state.
    static_cast<void>(second.wait_for(1s));
    release.set_value();
    const auto firstResult = first.get();
    const auto secondResult = second.get();
    session.Events().Unsubscribe(subscription);
    Check(published && firstResult.succeeded && secondResult.succeeded,
        "Concurrent preference fixture did not complete both real updates.");
    const auto view = session.UserPreferences();
    Check(!view.speechEnabled && !view.aiResponseReviewEnabled,
        "A completed preference update overwrote another live setting with a stale snapshot.");
}

void ReentrantPreferenceUpdateKeepsTheInnerLiveValue()
{
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    bool entered = false;
    revia::core::PreferenceResult inner;
    const auto subscription = session.Events().Subscribe([&](const auto& event)
    {
        if (event.component != "Response filters" || entered) return;
        entered = true;
        inner = session.SetPreference("speech.enabled", "off");
    });
    const auto outer = session.SetPreference("responseFilter.aiReviewEnabled", "off");
    session.Events().Unsubscribe(subscription);
    Check(entered && inner.succeeded && outer.succeeded, "Reentrant preference fixture did not complete both real updates.");
    const revia::core::PreferenceStore preferences(directory.root / "preferences.json");
    Check(preferences.Load().at("speech.enabled") == "false" && !session.IsSpeechEnabled(),
        "Reentrant setter did not save/apply its speech override.");
    const auto view = session.UserPreferences();
    Check(!view.speechEnabled && !view.aiResponseReviewEnabled,
        "Outer preference update overwrote the completed reentrant setter's live value.");
}
const std::vector<std::string> AddedPreferences = {"speechRecognition.requireWakeWord", "initiative.suppressWhenFullScreen",
    "conversation.selfInquiryEnabled", "conversation.iterativeInvestigationEnabled", "presence.enabled",
    "presence.requireAddressedStreamMessages", "presence.speakStreamReplies", "performance.enabled", "performance.interruptSongToSpeak"};

// The guest switches start off, so the subset rule can be seen.
void WriteCapabilities(const std::filesystem::path& root)
{
    Write(root / "Config/capabilities.json",
        {{"mode", "supervised"}, {"approvedRoots", json::array()}, {"createMissingApprovedRoots", false},
            {"guest", {{"enabled", false}, {"read", false}, {"write", false}, {"execute", false}, {"allowTaskExecution", false}}}});
}

void AddedPreferencesSetAndReset()
{
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Configure(directory.root);
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    const PreferenceStore preferences(directory.root / "preferences.json");
    for (const std::string& key : AddedPreferences)
    {
        const revia::core::PreferenceKey* definition = PreferenceStore::Find(key);
        Check(definition && definition->type == revia::core::PreferenceType::Boolean && !definition->description.empty() &&
                  !PreferenceStore::IsAuthoritySetting(key),
            key + " is not a described on/off comfort preference.");
        const bool configured = session.BooleanSettings().at(key).value;
        const auto set = session.SetPreference(key, configured ? "off" : "on");
        Check(set.succeeded, "Could not set " + key + ": " + set.message);
        Check(preferences.Load().contains(key) && session.BooleanSettings().at(key).value == !configured, key + " was not saved.");
        const auto reset = session.ResetPreference(key);
        Check(reset.succeeded && !preferences.Load().contains(key) && session.BooleanSettings().at(key).value == configured,
            key + " did not return to its configured value.");
    }
}

void RequireWakeWordNeedsWakeWords()
{
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Configure(directory.root, {{"speechRecognition", {{"enabled", false}, {"requireWakeWord", false}, {"wakeWords", json::array()}}}});
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    const auto refused = session.SetPreference("speechRecognition.requireWakeWord", "on");
    Check(!refused.succeeded && refused.message.find("wake words") != std::string::npos,
        "Requiring a wake word was accepted with no wake words configured.");
    Check(!PreferenceStore(directory.root / "preferences.json").Load().contains("speechRecognition.requireWakeWord"),
        "A refused wake-word requirement was saved.");
    Check(session.SetPreference("speechRecognition.requireWakeWord", "off").succeeded, "Turning the wake-word requirement off was refused.");
}

void EveryPreferenceCommitsOnlyItsOwnField()
{
    revia::tests::ScopedTestDirectory directory;
    PreferenceStore store(directory.root / "preferences.json");
    const appSettings defaults;
    const std::map<std::string, std::string> values = {{"speech.volume", "37"}, {"speech.rate", "3"}, {"initiative.maxPerHour", "7"},
        {"llm.temperature", "1.25"}, {"resources.usageSampleSeconds", "17"}, {"resources.voiceDevice", "cpu"}, {"activeProfile", "elsewhere"}};
    for (const revia::core::PreferenceKey& key : PreferenceStore::Writable())
    {
        const auto flag = revia::core::BooleanPreference(defaults, key.name);
        Check(flag.has_value() == (key.type == revia::core::PreferenceType::Boolean), key.name + " has no settings field.");
        const auto value = values.find(key.name);
        Check(flag || value != values.end(), "No fixture value for " + key.name + ".");
        Check(store.Set(key.name, flag ? (*flag ? "off" : "on") : value->second).succeeded, "Could not store " + key.name + ".");
    }
    appSettings source;
    store.Apply(source);
    appSettings copied;
    for (const revia::core::PreferenceKey& key : PreferenceStore::Writable())
    {
        std::string lowered = key.name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        Check(revia::core::CopySettingField(copied, source, lowered), "CopySettingField does not cover " + key.name + ".");
        if (const auto flag = revia::core::BooleanPreference(copied, key.name))
        {
            Check(*flag == revia::core::BooleanPreference(source, key.name) && *flag != revia::core::BooleanPreference(defaults, key.name),
                key.name + " was not copied from its own field.");
        }
    }
    Check(copied.speech.volume == source.speech.volume && copied.speech.rate == source.speech.rate &&
              copied.initiative.maxUtterancesPerHour == source.initiative.maxUtterancesPerHour &&
              copied.llm.temperature == source.llm.temperature && copied.resources.usageSampleSeconds == source.resources.usageSampleSeconds &&
              copied.resources.voice == source.resources.voice && copied.activeProfile == source.activeProfile,
        "A numeric or text preference was not copied from its own field.");
    appSettings single;
    Check(!revia::core::CopySettingField(single, source, "vision.enabled"), "CopySettingField accepted a key the store refuses.");
    Check(revia::core::CopySettingField(single, source, "speech.enabled") && single.speech.bEnabled == source.speech.bEnabled &&
              single.bargeIn.bEnabled == defaults.bargeIn.bEnabled && single.speech.volume == defaults.speech.volume,
        "Copying one preference changed another field.");
}

void RunningSessionKeepsRestartValuesUntilRestart()
{
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Configure(directory.root);
    WriteCapabilities(directory.root);
    const std::string configured = ReadText(directory.root / "Config/settings.json");
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    std::mutex changedMutex;
    std::vector<std::string> changed;
    const auto subscription = session.Events().Subscribe([&](const auto& event)
    {
        if (event.component != "Preferences" || event.phase != "Changed") return;
        std::lock_guard lock(changedMutex);
        changed.push_back(event.message);
    });
    Check(session.Start(), "Field-wise commit session did not start.");

    // Saved, but not what is running until the next start.
    Check(session.SetPreference("speech.speakGreeting", "on").succeeded, "Could not set the greeting preference.");
    auto states = session.BooleanSettings();
    Check(states.at("speech.speakGreeting").value && !states.at("speech.speakGreeting").running,
        "A restart-only preference changed the running settings.");
    const auto history = session.SetPreference("conversation.archiveEnabled", "on");
    states = session.BooleanSettings();
    Check(history.succeeded && history.message.find("next time") != std::string::npos && states.at("conversation.archiveEnabled").value &&
              !states.at("conversation.archiveEnabled").running,
        "Turning chat history on claimed to apply before a restart.");
    // Partly live: committed for the reader that runs now, the rest as Start read it.
    const bool singingAtStart = session.BooleanSettings().at("performance.enabled").running;
    Check(session.SetPreference("performance.enabled", singingAtStart ? "off" : "on").succeeded, "Could not set singing.");
    states = session.BooleanSettings();
    Check(states.at("performance.enabled").value != singingAtStart && states.at("performance.enabled").running == singingAtStart,
        "A partly live preference claimed to apply in full before a restart.");
    // The microphone (off at start here): turning it on waits for a restart.
    const auto microphone = session.SetPreference("speechRecognition.enabled", "on");
    states = session.BooleanSettings();
    Check(microphone.succeeded && microphone.message.find("next time") != std::string::npos &&
              states.at("speechRecognition.enabled").value && !states.at("speechRecognition.enabled").running,
        "Turning the microphone on claimed to apply before a restart.");
    // Live, read where it is used.
    Check(session.SetPreference("conversation.selfInquiryEnabled", "off").succeeded, "Could not set self-inquiry.");
    states = session.BooleanSettings();
    Check(!states.at("conversation.selfInquiryEnabled").value && !states.at("conversation.selfInquiryEnabled").running &&
              !session.ConversationSnapshot().bSelfInquiryEnabled,
        "A live preference did not reach the running settings.");
    // The command saves like the switch does, so the two cannot drift apart.
    Check(session.Submit("/bargein on").succeeded, "/bargein on failed.");
    states = session.BooleanSettings();
    Check(PreferenceStore(directory.root / "preferences.json").Load().at("bargeIn.enabled") == "true" &&
              states.at("bargeIn.enabled").value && states.at("bargeIn.enabled").running,
        "/bargein did not save the barge-in preference.");

    // The owner overlay: one live key, one that waits for a restart.
    Check(session.SetOwnerSetting("vision.continuousAwareness", true).succeeded, "Could not set a live owner setting.");
    Check(session.SetOwnerSetting("image.keepLoaded", true).succeeded, "Could not set a restart-only owner setting.");
    states = session.BooleanSettings();
    Check(states.at("vision.continuousAwareness").value && states.at("vision.continuousAwareness").running,
        "A live owner setting did not apply to the running session.");
    Check(states.at("image.keepLoaded").value && !states.at("image.keepLoaded").running,
        "A restart-only owner setting changed the running settings.");
    Check(std::filesystem::exists(directory.root / "RuntimeData/Settings/owner-settings.json") &&
              ReadText(directory.root / "Config/settings.json") == configured,
        "An owner setting was written somewhere other than the overlay.");
    Check(session.ResetOwnerSetting("image.keepLoaded").succeeded && !session.BooleanSettings().at("image.keepLoaded").value,
        "Resetting an owner setting did not return to settings.json.");
    Check(!session.SetOwnerSetting("llm.port", true).succeeded, "A key outside the overlay allowlist was accepted.");
    const auto cloud = session.SetOwnerSetting("llm.cloud.enabled", true);
    Check(!cloud.succeeded && cloud.message.find("baseUrl") != std::string::npos && !session.BooleanSettings().at("llm.cloud.enabled").available,
        "The cloud switch was offered without a configured endpoint.");
    Check(session.SetOwnerChoice("llm.cloud.useFor", "ask").succeeded && session.OwnerChoice("llm.cloud.useFor") == "ask",
        "The cloud use choice was not saved.");
    Check(!session.SetOwnerChoice("llm.cloud.useFor", "always").succeeded, "A cloud use choice outside the allowed values was saved.");

    // Session-only, persisted per companion, and the capability file.
    session.SetCloudSessionEnabled(false);
    Check(!session.IsCloudSessionEnabled() && !session.BooleanSettings().at("cloud.session").value, "The cloud session switch did not apply.");
    session.SetCloudSessionEnabled(true);
    Check(session.SetVirtualMachineEnabled(true).succeeded && session.IsVirtualMachineEnabled(), "The workspace start choice was not saved.");
    states = session.BooleanSettings();
    Check(states.at("virtualMachine.enabled").value && !states.at("virtualMachine.enabled").running,
        "The workspace start choice claimed to apply before a restart.");
    revia::policy::GuestCapabilitySettings guest = session.Capabilities().guest;
    guest.enabled = false;
    guest.execute = true;
    Check(session.SetGuestCapabilities(guest).succeeded && !session.Capabilities().guest.execute,
        "Guest program execution was stored without the guest permission it belongs to.");
    guest.enabled = true;
    guest.read = true;
    guest.execute = false;
    Check(session.SetGuestCapabilities(guest).succeeded && session.Capabilities().guest.enabled && session.Capabilities().guest.read &&
              !session.Capabilities().guest.write,
        "The guest switches were not saved as asked.");

    session.Events().Unsubscribe(subscription);
    std::lock_guard lock(changedMutex);
    for (const char* key : {"speech.speakGreeting", "conversation.selfInquiryEnabled", "bargeIn.enabled", "vision.continuousAwareness",
             "llm.cloud.useFor", "virtualMachine.enabled"})
    {
        Check(std::find(changed.begin(), changed.end(), key) != changed.end(), std::string("No Preferences change was published for ") + key + ".");
    }
}

// A configured cloud endpoint, one MCP server and continuous screen vision: everything
// Settings changes while Revia runs through a service of its own.
json LiveOwnerFixture()
{
    return {{"llm", {{"cloud", {{"baseUrl", "https://cloud.example/v1"}, {"model", "fixture-model"}}}}},
        {"vision", {{"enabled", true}, {"continuousAwareness", true}}},
        {"mcp", {{"servers", json::array({{{"id", "files"}, {"transport", "http"}, {"url", "http://127.0.0.1:9/files"}, {"enabled", false}}})}}}};
}

void LiveOwnerSettingsReachTheirServices()
{
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Configure(directory.root, LiveOwnerFixture());
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    Check(session.Start(), "Live owner session did not start.");

    // The cloud service's copy is what every turn asks, both ways.
    Check(!session.BooleanSettings().at("llm.cloud.enabled").running, "The cloud fixture started with the cloud on.");
    Check(session.SetOwnerSetting("llm.cloud.enabled", true).succeeded && session.BooleanSettings().at("llm.cloud.enabled").running,
        "Turning the cloud on in Settings did not reach the cloud service.");
    Check(session.SetOwnerSetting("llm.cloud.enabled", false).succeeded && !session.BooleanSettings().at("llm.cloud.enabled").running,
        "Turning the cloud off in Settings did not reach the cloud service.");
    static_cast<void>(session.Submit("/cloud on"));
    Check(session.IsCloudSessionEnabled() && !session.BooleanSettings().at("llm.cloud.enabled").running,
        "/cloud on brought back a cloud model Settings turned off.");
    Check(session.SetOwnerChoice("llm.cloud.useFor", "never").succeeded && Access::CloudUseFor(session) == "never",
        "The cloud use choice did not reach the cloud service.");

    // MCP servers are reconfigured, not only recorded.
    Check(Access::McpServerEnabled(session, "files") == false, "The MCP fixture server started enabled.");
    Check(session.SetOwnerSetting("mcp.servers[files].enabled", true).succeeded &&
              session.BooleanSettings().at("mcp.servers[files].enabled").running && Access::McpServerEnabled(session, "files") == true,
        "Turning an MCP server on did not reach the MCP manager.");
    Check(session.SetOwnerSetting("mcp.servers[files].enabled", false).succeeded && Access::McpServerEnabled(session, "files") == false,
        "Turning an MCP server off did not reach the MCP manager.");

    // What continuous awareness last saw goes with the switch that allowed it.
    Access::RecordScreenObservation(session, "A text editor fills the left monitor.");
    Check(!Access::ScreenContext(session).empty(), "The screen observation fixture was not offered.");
    Check(session.SetOwnerSetting("vision.enabled", false).succeeded && !session.BooleanSettings().at("vision.enabled").running,
        "Turning vision off did not reach the running session.");
    Check(Access::ScreenContext(session).empty() && Access::CachedScreenObservation(session).empty(),
        "Turning vision off left the last screen observation in every turn.");
    Check(session.SetOwnerSetting("vision.enabled", true).succeeded && Access::ScreenContext(session).empty(),
        "Turning vision back on revived an observation from before it was off.");
    Access::RecordScreenObservation(session, "A video call fills the right monitor.");
    Check(session.SetOwnerSetting("vision.continuousAwareness", false).succeeded && Access::ScreenContext(session).empty() &&
              Access::CachedScreenObservation(session).empty(),
        "Turning continuous awareness off left its last observation in use.");
}

// A setting that only means something inside another goes back to its safe value when that one
// is turned off, so turning the outer one on again never brings the inner one back unasked.
void AnInnerOwnerSettingGoesBackWithItsParent()
{
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Configure(directory.root, LiveOwnerFixture());
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    Check(session.Start(), "Inner owner setting session did not start.");

    Check(session.BooleanSettings().at("vision.continuousAwareness").running, "The fixture started without continuous awareness.");
    const auto visionOff = session.SetOwnerSetting("vision.enabled", false);
    auto states = session.BooleanSettings();
    Check(visionOff.succeeded && !states.at("vision.continuousAwareness").value && !states.at("vision.continuousAwareness").running &&
              visionOff.message.find("vision.continuousAwareness") != std::string::npos,
        "Keeping track of the screen survived vision being turned off.");
    Check(session.SetOwnerSetting("vision.enabled", true).succeeded && !session.BooleanSettings().at("vision.continuousAwareness").value,
        "Turning vision back on brought continuous awareness back unasked.");

    Check(session.SetOwnerSetting("localApi.enabled", true).succeeded && session.SetOwnerSetting("localApi.trustedPrivate", true).succeeded,
        "Could not trust the local API.");
    Check(session.SetOwnerSetting("localApi.enabled", false).succeeded && !session.BooleanSettings().at("localApi.trustedPrivate").value,
        "Trusting the local API survived the local API being turned off.");
    // Reset to settings.json, which has the local API off, counts as turning it off.
    Check(session.SetOwnerSetting("localApi.enabled", true).succeeded && session.SetOwnerSetting("localApi.trustedPrivate", true).succeeded,
        "Could not trust the local API again.");
    Check(session.ResetOwnerSetting("localApi.enabled").succeeded && !session.BooleanSettings().at("localApi.trustedPrivate").value,
        "Trusting the local API survived a reset that turned the local API off.");

    // Hard questions go back to "only when I ask" with the cloud; a choice that was already safe stays.
    Check(session.SetOwnerSetting("llm.cloud.enabled", true).succeeded && session.SetOwnerChoice("llm.cloud.useFor", "hardTurns").succeeded,
        "Could not send hard questions to the cloud.");
    Check(session.SetOwnerSetting("llm.cloud.enabled", false).succeeded && session.OwnerChoice("llm.cloud.useFor") == "ask",
        "Sending hard questions to the cloud survived the cloud being turned off.");
    Check(session.SetOwnerChoice("llm.cloud.useFor", "never").succeeded && session.SetOwnerSetting("llm.cloud.enabled", true).succeeded &&
              session.SetOwnerSetting("llm.cloud.enabled", false).succeeded && session.OwnerChoice("llm.cloud.useFor") == "never",
        "Turning the cloud off changed a cloud choice that was already safe.");
    Check(session.SetOwnerSetting("improvement.enabled", true).succeeded && session.SetOwnerSetting("improvement.verify", true).succeeded,
        "Could not build and test suggested improvements.");
    Check(session.SetOwnerSetting("improvement.enabled", false).succeeded && !session.BooleanSettings().at("improvement.verify").value,
        "Building model-written code survived self-improvement being turned off.");
    if (session.SetOwnerSetting("image.enabled", true).succeeded && session.SetOwnerSetting("image.offline", false).succeeded)
    {
        Check(session.SetOwnerSetting("image.enabled", false).succeeded && session.BooleanSettings().at("image.offline").value,
            "Picture downloads from the internet survived picture generation being turned off.");
    }

    Check(session.SetOwnerSetting("mcp.servers[files].enabled", true).succeeded &&
              session.SetOwnerSetting("mcp.servers[files].requireConfirmation", false).succeeded,
        "Could not stop the MCP server asking before each call.");
    Check(session.SetOwnerSetting("mcp.servers[files].enabled", false).succeeded &&
              session.BooleanSettings().at("mcp.servers[files].requireConfirmation").value,
        "Calls without a question survived their MCP server being disconnected.");
    // Turning something on leaves what sits inside it alone.
    Check(session.SetOwnerSetting("mcp.servers[files].requireConfirmation", false).succeeded &&
              session.SetOwnerSetting("mcp.servers[files].enabled", true).succeeded &&
              !session.BooleanSettings().at("mcp.servers[files].requireConfirmation").value,
        "Connecting an MCP server changed its per-call question.");
}

void TheScreenIsWithheldOnceVisionIsOffBeforeItsApplierRuns()
{
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Configure(directory.root, LiveOwnerFixture());
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    Check(session.Start(), "Screen withholding session did not start.");
    Access::RecordScreenObservation(session, "A spreadsheet fills the left monitor.");
    Check(!Access::ScreenContext(session).empty(), "The screen withholding fixture was not offered.");

    std::atomic<bool> entered{false};
    bool saved = false;
    std::string offered = "not looked at";
    std::string cached;
    const auto subscription = session.Events().Subscribe([&](const auto& event)
    {
        if (event.component != "Response filters" || entered.exchange(true)) return;
        // This applier is still running, so the vision applier that drops the observation
        // waits behind it, as it would behind one joining a worker. A turn asking for the
        // screen meanwhile must still get nothing.
        auto change = std::async(std::launch::async, [&] { return session.SetOwnerSetting("vision.enabled", false).succeeded; });
        saved = change.wait_for(2s) == std::future_status::ready && change.get();
        offered = Access::ScreenContext(session);
        cached = Access::CachedScreenObservation(session);
    });
    static_cast<void>(session.SetPreference("responseFilter.aiReviewEnabled", "off"));
    session.Events().Unsubscribe(subscription);
    Check(entered.load() && saved, "The vision fixture was not saved while another applier ran.");
    Check(!cached.empty(), "The vision applier ran before the fixture could look.");
    Check(offered.empty(), "The screen was offered after vision was turned off, before its applier ran.");
    Check(Access::CachedScreenObservation(session).empty(), "The vision applier never dropped the observation.");
}

void AProfileSaveFromSettingsNeverWaitsForATurn()
{
    // The Settings dialog's writes share one serial worker, so a profile switch saved during a
    // turn is refused at once rather than holding every later switch behind the reply.
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Configure(directory.root);
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    Check(session.Start(), "Profile save session did not start.");
    std::optional<revia::runtime::ProfileSummary> active = session.ActiveProfileSummary();
    Check(active.has_value() && !active->memoryEnabled, "The profile fixture did not start with memory off.");
    active->memoryEnabled = true;

    std::promise<void> held;
    std::promise<void> release;
    std::thread turn([&]
    {
        const auto lock = Access::HoldForeground(session);
        held.set_value();
        release.get_future().wait();
    });
    held.get_future().wait();
    auto attempt = std::async(std::launch::async, [&] { return session.TrySaveProfile(*active); });
    const bool answered = attempt.wait_for(2s) == std::future_status::ready;
    release.set_value();
    turn.join();
    const revia::runtime::ProfileOperationResult refused = attempt.get();
    Check(answered && !refused.succeeded && refused.message.find("middle of a turn") != std::string::npos,
        "A profile save from Settings waited for the turn instead of refusing: " + refused.message);
    Check(!session.ActiveProfileSummary()->memoryEnabled, "A refused profile save changed the profile.");
    Check(session.TrySaveProfile(*active).succeeded && session.ActiveProfileSummary()->memoryEnabled,
        "A profile save from Settings failed once the turn was over.");
}

void LivePreferencesReachTheirServices()
{
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Configure(directory.root, {{"speechRecognition", {{"enabled", false}, {"requireWakeWord", false}, {"wakeWords", {"revia"}}}},
                                  {"presence", {{"enabled", false}, {"avatarBridgeEnabled", false}, {"externalAdaptersEnabled", false}}}});
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    Check(session.Start(), "Live preference session did not start.");

    Check(Access::AcceptsUnnamedSpeech(session, "what time is it"), "The wake-word fixture started requiring her name.");
    Check(session.SetPreference("speechRecognition.requireWakeWord", "on").succeeded &&
              !Access::AcceptsUnnamedSpeech(session, "what time is it") && Access::AcceptsUnnamedSpeech(session, "revia, what time is it"),
        "Requiring a wake word did not reach the addressee gate.");
    Check(session.SetPreference("speechRecognition.requireWakeWord", "off").succeeded && Access::AcceptsUnnamedSpeech(session, "what time is it"),
        "Dropping the wake-word requirement did not reach the addressee gate.");

    Check(!session.Presence().enabled, "The presence fixture started enabled.");
    Check(session.SetPreference("presence.enabled", "on").succeeded && session.Presence().enabled, "Turning presence on did not restart it.");
    Check(session.SetPreference("presence.enabled", "off").succeeded && !session.Presence().enabled, "Turning presence off did not restart it.");
}

void OwnerChoicesSavedDuringStartupApply()
{
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Configure(directory.root, LiveOwnerFixture());
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    bool written = false;
    std::vector<bool> saved;
    const auto subscription = session.Events().Subscribe([&](const auto& event)
    {
        if (event.component != "Response filters" || written || session.IsStarted()) return;
        // Start has read the settings and configured MCP, and nothing it starts runs yet.
        written = true;
        saved = {session.SetOwnerSetting("mcp.servers[files].enabled", true).succeeded,
            session.SetOwnerSetting("llm.cloud.enabled", true).succeeded, session.SetOwnerSetting("vision.enabled", false).succeeded};
    });
    Check(session.Start(), "Startup-write session did not start.");
    session.Events().Unsubscribe(subscription);
    Check(written && saved == std::vector<bool>{true, true, true}, "The startup-write fixture did not save its changes.");
    const auto states = session.BooleanSettings();
    Check(states.at("mcp.servers[files].enabled").running && Access::McpServerEnabled(session, "files") == true,
        "An MCP change saved during startup was lost until the next start.");
    Check(states.at("llm.cloud.enabled").running, "A cloud change saved during startup was lost until the next start.");
    Check(!states.at("vision.enabled").running, "A vision change saved during startup was lost until the next start.");
}

void APreferenceChangeNeverWaitsForAnotherApplier()
{
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Configure(directory.root);
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    std::atomic<bool> entered{false};
    std::future<revia::core::PreferenceResult> other;
    std::future_status waited = std::future_status::deferred;
    const auto subscription = session.Events().Subscribe([&](const auto& event)
    {
        if (event.component != "Response filters" || entered.exchange(true)) return;
        // This applier is still running. A change from another thread returns without waiting
        // for it, as one from a turn holding operationMutex must while an applier joins a worker.
        other = std::async(std::launch::async, [&] { return session.SetPreference("bargeIn.enabled", "on"); });
        waited = other.wait_for(2s);
    });
    const auto outer = session.SetPreference("responseFilter.aiReviewEnabled", "off");
    session.Events().Unsubscribe(subscription);
    Check(entered.load() && other.valid(), "The applier fixture did not run.");
    const auto inner = other.get();
    Check(waited == std::future_status::ready, "A preference change waited for an applier running on another thread.");
    Check(outer.succeeded && inner.succeeded && session.IsBargeInEnabled(), "The change made during another applier was not applied.");
}

void ConcurrentWritersAndReadersStayConsistent()
{
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Configure(directory.root);
    ReviaSession session;
    Access::UsePreferences(session, directory.root);
    Check(session.Start(), "Concurrency stress session did not start.");
    std::atomic<bool> writing{true};
    std::atomic<bool> readFailed{false};
    std::atomic<int> reads{0};
    std::thread reader([&]
    {
        while (writing.load())
        {
            static_cast<void>(session.ConversationSnapshot());
            const auto states = session.BooleanSettings();
            const auto found = states.find("conversation.selfInquiryEnabled");
            if (found == states.end() || !found->second.available)
                readFailed.store(true);
            ++reads;
        }
    });
    bool succeeded = true;
    for (int index = 0; index < 200; ++index)
    {
        const char* value = index % 2 == 0 ? "on" : "off";
        succeeded = session.SetPreference("conversation.selfInquiryEnabled", value).succeeded && succeeded;
        succeeded = session.SetPreference(index % 3 == 0 ? "presence.speakStreamReplies" : "responseFilter.aiReviewEnabled", value).succeeded &&
                    succeeded;
    }
    writing.store(false);
    reader.join();
    Check(succeeded && !readFailed.load() && reads.load() > 0, "Concurrent preference writes or reads failed.");
    // The last writes: self-inquiry off (199), AI review off (199), stream replies on (198).
    const auto states = session.BooleanSettings();
    Check(!states.at("conversation.selfInquiryEnabled").value && !states.at("conversation.selfInquiryEnabled").running &&
              !session.ConversationSnapshot().bSelfInquiryEnabled,
        "Self-inquiry did not settle on the last write.");
    Check(!states.at("responseFilter.aiReviewEnabled").running && !session.UserPreferences().aiResponseReviewEnabled,
        "AI review did not settle on the last write.");
    Check(states.at("presence.speakStreamReplies").value && states.at("presence.speakStreamReplies").running,
        "Speaking stream replies did not settle on the last write.");
}
} // namespace

void RunPreferenceResetTests()
{
    LiveUnsetRestoresOnlyTheConfiguredKey();
    FailedClearPreservesLiveAndDurableOverride();
    ConcurrentPreferenceUpdatesKeepBothLiveValues();
    ReentrantPreferenceUpdateKeepsTheInnerLiveValue();
    AddedPreferencesSetAndReset();
    RequireWakeWordNeedsWakeWords();
    EveryPreferenceCommitsOnlyItsOwnField();
    RunningSessionKeepsRestartValuesUntilRestart();
    LiveOwnerSettingsReachTheirServices();
    AnInnerOwnerSettingGoesBackWithItsParent();
    LivePreferencesReachTheirServices();
    OwnerChoicesSavedDuringStartupApply();
    APreferenceChangeNeverWaitsForAnotherApplier();
    TheScreenIsWithheldOnceVisionIsOffBeforeItsApplierRuns();
    AProfileSaveFromSettingsNeverWaitsForATurn();
    ConcurrentWritersAndReadersStayConsistent();
    std::cout << "Preference reset tests passed.\n";
}
