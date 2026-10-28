#include "Core/preferenceStore.h"
#include "Core/configManager.h"
#include "Initiative/attentionPolicy.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace
{
using revia::core::PreferenceStore;
using revia::initiative::AttentionContext;
using revia::initiative::AttentionPolicy;
using revia::initiative::AttentionVerdict;
using namespace std::chrono_literals;

void CheckMode(const bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct ModeFixture
{
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("revia-initiative-mode-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    PreferenceStore store{root / "preferences.json"};
    appSettings settings;
    const std::chrono::system_clock::time_point now{1h};

    ModeFixture()
    {
        settings.initiative.bEnabled = true;
        settings.initiative.cooldownSeconds = 900;
        settings.initiative.dismissalCooldownSeconds = 3600;
        settings.initiative.maxUtterancesPerHour = 2;
    }
    ~ModeFixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    void Select(const std::string& mode)
    {
        CheckMode(store.Set("initiative.mode", mode).succeeded, "The initiative mode preference was refused.");
        store.Apply(settings);
    }
    AttentionContext At(const std::chrono::seconds elapsed = 0s) const
    {
        AttentionContext context;
        context.now = now + elapsed;
        context.sinceLastInput = 1h;
        return context;
    }
};

void TestFocusSuppressesOptionalInitiative()
{
    ModeFixture fixture;
    fixture.Select("focus");
    const AttentionPolicy policy(fixture.settings.initiative);
    CheckMode(policy.Evaluate(1.0F, fixture.At()) != AttentionVerdict::Speak,
        "Focus allowed an optional initiative.");
    CheckMode(revia::initiative::ToString(policy.Evaluate(1.0F, fixture.At())) == "focus mode",
        "Focus suppression did not explain itself.");
}

void TestNormalPreservesTheConfiguredCooldown()
{
    ModeFixture fixture;
    fixture.store.Apply(fixture.settings);
    AttentionPolicy policy(fixture.settings.initiative);
    policy.RecordSpoken(fixture.now);
    CheckMode(policy.Evaluate(1.0F, fixture.At(450s)) == AttentionVerdict::Cooldown,
        "An old configuration no longer uses its configured cooldown.");
    fixture.Select("normal");
    policy.UpdateSettings(fixture.settings.initiative);
    CheckMode(policy.Evaluate(1.0F, fixture.At(899s)) == AttentionVerdict::Cooldown &&
            policy.Evaluate(1.0F, fixture.At(900s)) == AttentionVerdict::Speak,
        "Normal mode changed the configured cooldown or cleared its history.");
}

void TestTalkativeUsesEarlierOpeningsWithinTheSameBudget()
{
    ModeFixture fixture;
    fixture.Select("talkative");
    AttentionPolicy policy(fixture.settings.initiative);
    policy.RecordSpoken(fixture.now);
    CheckMode(policy.Evaluate(1.0F, fixture.At(449s)) == AttentionVerdict::Cooldown &&
            policy.Evaluate(1.0F, fixture.At(450s)) == AttentionVerdict::Speak,
        "Talkative did not shorten only the ordinary cooldown.");
    policy.RecordSpoken(fixture.now + 450s);
    CheckMode(policy.Evaluate(1.0F, fixture.At(900s)) == AttentionVerdict::HourlyBudget &&
            policy.EffectiveHourlyBudget() == 2 && fixture.settings.initiative.cooldownSeconds == 900 &&
            fixture.settings.initiative.maxUtterancesPerHour == 2,
        "Talkative widened or rewrote the configured interruption budget.");
}

void TestModesRetainHardGatesAndDismissalHistory()
{
    ModeFixture fixture;
    fixture.Select("talkative");
    AttentionPolicy policy(fixture.settings.initiative);
    auto context = fixture.At();
    context.sinceLastInput = 0s;
    CheckMode(policy.Evaluate(1.0F, context) == AttentionVerdict::UserIsBusy,
        "Talkative interrupted user input.");
    context = fixture.At();
    context.foregroundIsFullScreen = true;
    CheckMode(policy.Evaluate(1.0F, context) == AttentionVerdict::FullScreen,
        "Talkative bypassed fullscreen suppression.");
    context = fixture.At();
    context.foregroundIsExcluded = true;
    CheckMode(policy.Evaluate(1.0F, context) == AttentionVerdict::ExcludedApplication,
        "Talkative bypassed an excluded application.");
    context = fixture.At();
    context.inCall = true;
    CheckMode(policy.Evaluate(1.0F, context) == AttentionVerdict::InCall,
        "Talkative spoke over a call.");
    CheckMode(policy.Evaluate(0.1F, fixture.At()) == AttentionVerdict::BelowConfidence,
        "Talkative bypassed the confidence threshold.");
    policy.RecordDismissed(fixture.now);
    fixture.Select("focus");
    policy.UpdateSettings(fixture.settings.initiative);
    fixture.Select("normal");
    policy.UpdateSettings(fixture.settings.initiative);
    CheckMode(policy.Evaluate(1.0F, fixture.At(3599s)) == AttentionVerdict::DismissalCooldown,
        "Changing modes erased dismissal history.");
}

void TestZeroHourlyBudgetRemainsNeverAfterPoorPrecision()
{
    ModeFixture fixture;
    fixture.Select("talkative");
    fixture.settings.initiative.maxUtterancesPerHour = 0;
    AttentionPolicy policy(fixture.settings.initiative);
    for (int index = 0; index < 5; ++index)
        policy.RecordDismissed(fixture.now);
    CheckMode(policy.IsRateReduced() && policy.EffectiveHourlyBudget() == 0 &&
            policy.Evaluate(1.0F, fixture.At(3600s)) == AttentionVerdict::ReducedForPrecision,
        "Poor precision changed the zero hourly budget from never to an opening.");
}

void TestModePersistenceValidationAndNarrowCopy()
{
    ModeFixture fixture;
    fixture.Select("focus");
    CheckMode(fixture.store.Load().at("initiative.mode") == "focus", "The mode did not round-trip.");
    CheckMode(!fixture.store.Set("initiative.mode", "always_interrupt").succeeded,
        "An unknown mode was accepted.");
    appSettings destination;
    destination.initiative.bEnabled = true;
    destination.initiative.maxUtterancesPerHour = 7;
    destination.vision.bContinuousAwareness = false;
    CheckMode(revia::core::CopySettingField(destination, fixture.settings, "INITIATIVE.MODE"),
        "The mode was not registered for live preference copying.");
    CheckMode(AttentionPolicy(destination.initiative).Evaluate(1.0F, fixture.At()) != AttentionVerdict::Speak &&
            destination.initiative.maxUtterancesPerHour == 7 && !destination.vision.bContinuousAwareness,
        "Copying the mode changed another preference or capability.");
    CheckMode(fixture.store.Clear("initiative.mode").succeeded, "The mode could not be reset.");
    appSettings reset;
    reset.initiative.bEnabled = true;
    fixture.store.Apply(reset);
    CheckMode(AttentionPolicy(reset.initiative).Evaluate(1.0F, fixture.At()) == AttentionVerdict::Speak,
        "Resetting the mode did not restore normal initiative.");
}

void TestComfortModeClassificationKeepsAuthoritySettingsExcluded()
{
    ModeFixture fixture;
    CheckMode(!PreferenceStore::IsAuthoritySetting("initiative.mode") &&
            !PreferenceStore::IsAuthoritySetting("INITIATIVE.MODE"),
        "The exact initiative pacing preference was classified as authority.");
    for (const char* name : {"mode", "capabilities.mode", "guest.mode", "operator.mode", "policy.mode",
        "initiative.unknownMode", "initiative.mode.extra", "capabilities.initiative.mode"})
    {
        CheckMode(PreferenceStore::IsAuthoritySetting(name), "A non-comfort mode key lost its authority classification.");
        const auto refused = fixture.store.Set(name, "focus");
        CheckMode(PreferenceStore::Find(name) == nullptr && !refused.succeeded &&
                refused.message.find("permitted") != std::string::npos,
            "The initiative mode exception made an authority or unknown mode key writable.");
    }
}

void TestComfortModeAcceptsOnlyItsClosedPacingValues()
{
    ModeFixture fixture;
    for (const char* mode : {"normal", "talkative", "focus"})
        CheckMode(fixture.store.Set("initiative.mode", mode).succeeded &&
                fixture.store.Load().at("initiative.mode") == mode,
            "A supported initiative pacing value did not persist.");
    const auto saved = fixture.store.Load();
    for (const char* value : {"", "free", "owner_full_access", "always_interrupt", "FOCUS", "focus "})
        CheckMode(!fixture.store.Set("initiative.mode", value).succeeded && fixture.store.Load() == saved,
            "An invalid initiative mode was accepted or changed the last saved pacing value.");
}

void TestConfiguredAndMalformedModesKeepSafeDefaults()
{
    ModeFixture fixture;
    std::filesystem::create_directories(fixture.root);
    const auto settingsPath = fixture.root / "settings.json";
    const configManager config(settingsPath.string(), (fixture.root / "Profiles").string());
    constexpr const char* quietConfiguration =
        "{\"llm\":{\"backend\":\"None\",\"autoStartServer\":false,\"visionEnabled\":false},"
        "\"intelligence\":{\"enabled\":false},\"embedding\":{\"enabled\":false},"
        "\"speech\":{\"enabled\":false,\"backend\":\"WindowsSapi\"},"
        "\"initiative\":{\"enabled\":true";
    for (const auto& [serialized, expected] : {
        std::pair{"\"FoCuS\"", "focus"}, {"\"talkative\"", "talkative"},
        {"\"normal\"", "normal"}, {"\"unknown\"", "normal"}, {"123", "normal"}})
    {
        std::ofstream(settingsPath) << quietConfiguration << ",\"mode\":" << serialized << "}}";
        appSettings loaded;
        CheckMode(config.LoadSettings(loaded), "Loading a mode configuration failed.");
        CheckMode(loaded.initiative.mode == expected, "A configured or unknown mode did not normalize safely.");
    }
    std::ofstream(settingsPath) << quietConfiguration << "}}";
    appSettings oldConfiguration;
    CheckMode(config.LoadSettings(oldConfiguration) && oldConfiguration.initiative.mode == "normal",
        "A configuration without a mode did not preserve normal pacing.");

    std::ofstream(fixture.root / "preferences.json") <<
        "{\"initiative.mode\":\"always_interrupt\"}";
    fixture.store.Apply(oldConfiguration);
    CheckMode(oldConfiguration.initiative.mode == "normal",
        "An invalid stored mode widened normal pacing.");
}
}

void RunInitiativeModeTests()
{
    TestFocusSuppressesOptionalInitiative();
    TestNormalPreservesTheConfiguredCooldown();
    TestTalkativeUsesEarlierOpeningsWithinTheSameBudget();
    TestModesRetainHardGatesAndDismissalHistory();
    TestZeroHourlyBudgetRemainsNeverAfterPoorPrecision();
    TestModePersistenceValidationAndNarrowCopy();
    TestComfortModeClassificationKeepsAuthoritySettingsExcluded();
    TestComfortModeAcceptsOnlyItsClosedPacingValues();
    TestConfiguredAndMalformedModesKeepSafeDefaults();
    std::cout << "Initiative mode tests passed.\n";
}

#ifdef REVIA_INITIATIVE_MODE_TEST_MAIN
int main()
{
    try
    {
        RunInitiativeModeTests();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
#endif
