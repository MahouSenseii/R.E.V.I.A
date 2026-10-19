#include "desktopSuites.h"
#include "settingsSessionBackend.h"

#include "Runtime/reviaSession.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

// Revia.SettingsBackend: the Settings dialog's backend over a real, started session, as the
// window builds it. Writes run inline here, so each one has answered before the next line, and
// every store is written once through the adapter and read back through it. Keys a store does
// not own, such as a capability that is not the Ubuntu workspace's, must be refused without
// touching anything.
namespace
{
using revia::desktop::SettingKeyRef;
using revia::desktop::SettingRead;
using revia::desktop::SettingStore;
using revia::desktop::SettingWrite;
using json = nlohmann::json;

void WriteJson(const std::filesystem::path& path, const json& value)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value.dump(2);
}

// The test binary's working directory is a fresh temporary root with an empty Config.
void WriteFixture(const std::filesystem::path& root)
{
    WriteJson(root / "Config/settings.json",
        {{"activeProfile", "fixture"},
            {"llm", {{"backend", "None"}, {"autoStartServer", false}, {"visionEnabled", false}, {"modelPath", (root / "absent.gguf").string()},
                        {"mediaPath", (root / "RuntimeData/Vision").string()}}},
            {"intelligence", {{"enabled", false}}}, {"embedding", {{"enabled", false}, {"autoStartServer", false}}},
            {"speech", {{"enabled", true}, {"backend", "WindowsSapi"}, {"speakGreeting", false}, {"voiceDataPath", (root / "RuntimeData/Voices").string()}}},
            {"speechRecognition", {{"enabled", false}}}, {"presence", {{"enabled", false}, {"externalAdaptersEnabled", false}}},
            {"vision", {{"enabled", true}}}, {"perception", {{"enabled", false}}}, {"initiative", {{"enabled", false}}},
            {"conversation", {{"archiveEnabled", false}}}, {"image", {{"enabled", false}}}, {"resources", {{"startupSampleSeconds", 0}}}});
    WriteJson(root / "Config/Profiles/fixture.json", {{"id", "fixture"}, {"displayName", "Fixture Profile"}, {"systemPrompt", "Settings backend fixture."}, {"memoryEnabled", false}});
    WriteJson(root / "Config/capabilities.json",
        {{"mode", "supervised"}, {"approvedRoots", json::array()}, {"createMissingApprovedRoots", false},
            {"guest", {{"enabled", false}, {"read", false}, {"write", false}, {"execute", false}, {"allowTaskExecution", false}}}});
}

int RunSettingsBackendSuite(int, char**)
{
    int failures = 0;
    const auto expect = [&failures](const bool condition, const std::string& message)
    {
        if (!condition)
        {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    };

    const std::filesystem::path root = std::filesystem::current_path();
    WriteFixture(root);
    revia::runtime::ReviaSession session;
    if (!session.Start())
    {
        std::cerr << "FAIL: the settings backend session did not start\n";
        return 1;
    }

    bool draftDirty = false;
    int profileRefreshes = 0;
    int posted = 0;
    revia::desktop::SessionBackendDeps deps;
    deps.session = [&session]() -> revia::runtime::ReviaSession& { return session; };
    deps.runInBackground = [](std::function<void()> work) { work(); };
    deps.postToUi = [&posted](std::function<void()> callback)
    {
        ++posted;
        callback();
    };
    deps.profileDraftDirty = [&draftDirty]() { return draftDirty; };
    deps.afterProfileWrite = [&profileRefreshes]() { ++profileRefreshes; };
    const revia::desktop::SettingsBackend backend = revia::desktop::MakeSessionSettingsBackend(deps);

    std::optional<SettingWrite> answer;
    const auto done = [&answer](SettingWrite result) { answer = result; };
    const auto write = [&](const SettingKeyRef& ref, const bool value)
    {
        answer.reset();
        backend.write(ref, value, done);
        return answer.value_or(SettingWrite{false, QStringLiteral("no answer")});
    };
    const auto read = [&](const SettingKeyRef& ref) { return backend.readAll({ref}).at(ref.key); };

    // Only the Ubuntu workspace's own keys reach a guest permission. Every other capability
    // ends in .enabled too, and must be refused as unknown, not mapped to guest.enabled.
    const SettingKeyRef guestEnabled{"capabilities.guest.enabled", SettingStore::Capability};
    for (const char* key : {"capabilities.camera.enabled", "capabilities.internet.enabled", "capabilities.process.allowTaskExecution"})
    {
        const SettingKeyRef other{key, SettingStore::Capability};
        const SettingRead seen = read(other);
        expect(!seen.available && seen.unavailableReason.contains(QStringLiteral("does not know")),
            std::string(key) + " was read as an Ubuntu workspace permission");
        const SettingWrite refused = write(other, true);
        expect(!refused.succeeded && refused.message.contains(QStringLiteral("does not know")), std::string(key) + " was written");
        expect(!session.Capabilities().guest.enabled && !session.Capabilities().guest.allowTaskExecution,
            std::string(key) + " changed an Ubuntu workspace permission");
    }
    expect(read(guestEnabled).available && !read(guestEnabled).value, "the guest switch did not read back as off");
    expect(write(guestEnabled, true).succeeded && session.Capabilities().guest.enabled && read(guestEnabled).value,
        "the guest switch was not saved through the adapter");
    const SettingKeyRef guestExecute{"capabilities.guest.execute", SettingStore::Capability};
    expect(write(guestExecute, true).succeeded && session.Capabilities().guest.execute, "a guest sub-permission was not saved");
    expect(write(guestEnabled, false).succeeded && !session.Capabilities().guest.execute,
        "turning the guest off kept a permission that belongs to it");

    // A preference that waits for a restart shows the saved value beside the running one.
    const SettingKeyRef greeting{"speech.speakGreeting", SettingStore::Preference};
    expect(write(greeting, true).succeeded && read(greeting).value && !read(greeting).running,
        "a restart-only preference did not read back as saved but not running");
    answer.reset();
    backend.reset(greeting, done);
    expect(answer && answer->succeeded && !read(greeting).value, "resetting a preference did not return it to settings.json");

    // The owner overlay, live: running follows at once.
    const SettingKeyRef vision{"vision.enabled", SettingStore::SettingsFile};
    expect(write(vision, false).succeeded && !read(vision).value && !read(vision).running, "a live owner switch did not apply");
    answer.reset();
    backend.reset(vision, done);
    expect(answer && answer->succeeded && read(vision).value && read(vision).running, "resetting an owner switch did not apply");
    answer.reset();
    backend.writeChoice("llm.cloud.useFor", QStringLiteral("never"), done);
    expect(answer && answer->succeeded && backend.readChoice("llm.cloud.useFor") == QStringLiteral("never"), "the cloud use choice was not saved");
    answer.reset();
    backend.writeChoice("llm.cloud.useFor", QStringLiteral("always"), done);
    expect(answer && !answer->succeeded, "a cloud use choice outside the allowed values was saved");

    // Session-only switches and the workspace start file.
    const SettingKeyRef cloudSession{"cloud.session", SettingStore::Session};
    expect(write(cloudSession, false).succeeded && !session.IsCloudSessionEnabled() && !read(cloudSession).value,
        "the cloud session switch did not apply");
    expect(write(cloudSession, true).succeeded && session.IsCloudSessionEnabled(), "the cloud session switch did not come back");
    const SettingKeyRef paused{"perception.paused", SettingStore::Session};
    expect(!read(paused).available && !write(paused, true).succeeded, "pausing app awareness was offered while it does not run");
    const SettingKeyRef machine{"virtualMachine.enabled", SettingStore::VirtualMachine};
    expect(write(machine, true).succeeded && read(machine).value && !read(machine).running && session.IsVirtualMachineEnabled(),
        "the workspace start choice did not read back as waiting for a restart");

    // The Chat section's profile group is titled with the active profile's own name.
    expect(backend.readChoice("profile.displayName") == QStringLiteral("Fixture Profile"),
        "the profile group's name was not the active profile's: \"" + backend.readChoice("profile.displayName").toStdString() + "\"");

    // Profile switches: never over an unsaved draft, and the editor refreshes after a save.
    const SettingKeyRef memory{"profile.memoryEnabled", SettingStore::Profile};
    draftDirty = true;
    const SettingWrite overDraft = write(memory, true);
    expect(!overDraft.succeeded && overDraft.message.contains(QStringLiteral("Profiles")) && profileRefreshes == 0 && !read(memory).value,
        "a profile switch was saved over unsaved Profiles edits");
    draftDirty = false;
    expect(write(memory, true).succeeded && read(memory).value && profileRefreshes == 1, "a profile switch was not saved and refreshed");
    // The dialog offers the value only once its override is on, as the profile file keeps it.
    expect(write({"profile.hasTemperatureOverride", SettingStore::Profile}, true).succeeded && profileRefreshes == 2,
        "a profile override switch was not saved");
    answer.reset();
    backend.writeProfileValue("temperature", 0.5, done);
    expect(answer && answer->succeeded && backend.readProfileValue("temperature") == 0.5 && profileRefreshes == 3,
        "a profile value was not saved: " + (answer ? answer->message.toStdString() : std::string("no answer")));

    // Every answer, refusals included, went through postToUi.
    const SettingWrite unknown = write({"window.alwaysOnTop", SettingStore::Shell}, true);
    expect(!unknown.succeeded && posted > 0, "a shell key reached the session backend");

    session.Stop();
    if (failures == 0)
        std::cout << "Settings backend passed.\n";
    return failures;
}
}

REVIA_DESKTOP_SUITE("settingsBackend", RunSettingsBackendSuite);
