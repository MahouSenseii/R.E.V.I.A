#include "reviaSessionTestAccess.h"

#include <fstream>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace
{
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

void Configure(const std::filesystem::path& root)
{
    Write(root / "Config/settings.json", {
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
    });
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
} // namespace

void RunPreferenceResetTests()
{
    LiveUnsetRestoresOnlyTheConfiguredKey();
    FailedClearPreservesLiveAndDurableOverride();
    ConcurrentPreferenceUpdatesKeepBothLiveValues();
    ReentrantPreferenceUpdateKeepsTheInnerLiveValue();
    std::cout << "Preference reset tests passed.\n";
}
