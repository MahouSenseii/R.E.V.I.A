#include "Audit/contentDigest.h"
#include "Core/appSettings.h"
#include "Core/configManager.h"
#include "Core/profile.h"
#include "Core/runtimePath.h"
#include "Core/settingsFileEditor.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Json = nlohmann::json;
constexpr const char* VisionKey = "llm.visionEnabled";

void Require(const bool value, const std::string& reason)
{
    if (!value)
        throw std::runtime_error(reason);
}

std::string Read(const std::filesystem::path& path, const std::size_t limit)
{
    std::ifstream input(path, std::ios::binary);
    Require(input.good(), "The explicitly selected file could not be read: " + path.string());
    std::string bytes(limit + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    Require(input.eof() && !input.bad() && bytes.size() <= limit, "A selected input exceeded its bound.");
    return bytes;
}

std::filesystem::path Artifact(const std::filesystem::path& install, const std::string& configured)
{
    const std::filesystem::path value(configured);
    const auto found = value.is_absolute() ? value : revia::core::FindInstalledArtifact(install, value);
    Require(!found.empty() && std::filesystem::is_regular_file(found), "An existing configured model/projector is unavailable.");
    return std::filesystem::canonical(found);
}

struct Snapshot
{
    Json report;
    Json ownerValues;
};

Snapshot Capture(const std::filesystem::path& settingsPath, const std::filesystem::path& profiles, const std::filesystem::path& overlay)
{
    Json owner = {{"schemaVersion", 1}, {"values", Json::object()}};
    std::vector<std::string> warnings;
    const auto state = revia::core::ReadOwnerOverlay(overlay, owner, &warnings);
    Require(state == revia::core::OwnerOverlayRead::Missing || state == revia::core::OwnerOverlayRead::Usable,
        "The owner overlay is not currently readable and usable; this helper will not repair or replace it.");
    const bool exists = std::filesystem::exists(overlay);
    if (exists)
        Require(Json::parse(Read(overlay, revia::core::kMaxOwnerOverlayBytes)) == owner && warnings.empty(),
            "The selected overlay contains unsupported data; preserve it for separate review rather than discard entries.");
    if (state == revia::core::OwnerOverlayRead::Missing)
        owner = {{"schemaVersion", 1}, {"values", Json::object()}};
    configManager config(settingsPath.string(), profiles.string());
    config.SetOwnerOverlay(overlay.string());
    appSettings settings;
    aiProfile profile;
    warnings.clear();
    Require(config.LoadSettings(settings, warnings) && warnings.empty(), "The selected effective settings did not load without warnings.");
    Require(configManager::IsSafeProfileId(settings.activeProfile) && config.LoadProfile(settings.activeProfile, profile),
        "The actual selected profile could not be read.");
    const auto profilePath = std::filesystem::canonical(profiles / (settings.activeProfile + ".json"));
    const auto install = settingsPath.parent_path().parent_path();
    Json other = owner.at("values");
    other.erase(VisionKey);
    Snapshot result;
    result.ownerValues = owner.at("values");
    result.report = {{"settingsPath", settingsPath.string()}, {"profileDirectory", profiles.string()}, {"overlayPath", overlay.string()},
        {"settingsSha256", revia::audit::ContentDigest(Read(settingsPath, 1048576))}, {"profilePath", profilePath.string()},
        {"profileSha256", revia::audit::ContentDigest(Read(profilePath, 1048576))}, {"overlayExists", exists},
        {"overlaySha256", exists ? revia::audit::ContentDigest(Read(overlay, revia::core::kMaxOwnerOverlayBytes)) : "missing"},
        {"otherOwnerChoicesSha256", revia::audit::ContentDigest(other.dump())}, {"otherOwnerChoiceCount", other.size()},
        {"ownerMainVision", result.ownerValues.contains(VisionKey) ? result.ownerValues.at(VisionKey) : Json(nullptr)},
        {"effectiveMainVision", settings.llm.bVisionEnabled}, {"effectiveImageMinTokens", settings.llm.imageMinTokens},
        {"contextSize", settings.llm.contextSize}, {"configuredMaxTokens", settings.llm.maxTokens},
        {"configuredThinkingTokens", settings.llm.thinkingTokens},
        {"effectiveTemperature", profile.bHasTemperatureOverride ? profile.temperature : settings.llm.temperature},
        {"modelPath", Artifact(install, settings.llm.modelPath).string()},
        {"projectorPath", Artifact(install, settings.llm.multimodalProjectorPath).string()}, {"screenVision", settings.vision.bEnabled},
        {"continuousScreenAwareness", settings.vision.bContinuousAwareness}};
    return result;
}
}

int main(int argc, char** argv)
{
    if ((argc != 6 && argc != 7) || (std::string(argv[1]) != "dry-run" && std::string(argv[1]) != "apply"))
    {
        std::cerr << "Usage: ReviaMainVisionSetting <dry-run|apply> <settings.json> <profiles-directory> <owner-overlay> "
                     "<new-receipt.json> [dry-run-receipt]\n";
        return 2;
    }
    try
    {
        const bool apply = std::string(argv[1]) == "apply";
        Require(apply == (argc == 7), "Apply requires its explicit earlier dry-run receipt; dry-run takes no expected receipt.");
        const auto settings = std::filesystem::canonical(argv[2]);
        const auto profiles = std::filesystem::canonical(argv[3]);
        const auto overlay = std::filesystem::absolute(argv[4]).lexically_normal();
        const auto receipt = std::filesystem::absolute(argv[5]);
        Require(!std::filesystem::exists(receipt), "Existing receipts are never replaced.");
        const auto before = Capture(settings, profiles, overlay);
        Json report = {{"mode", apply ? "apply" : "dry-run"}, {"key", VisionKey}, {"requestedValue", true}, {"before", before.report},
            {"normalSessionStarted", false}, {"restartRequired", true}};
        if (apply)
        {
            const auto expected = Json::parse(Read(std::filesystem::canonical(argv[6]), 1048576));
            Require(expected.value("mode", "") == "dry-run" && expected.at("before") == before.report,
                "The selected settings, profile or owner choices changed after dry-run; take a new snapshot before applying.");
            if (!before.report.at("effectiveMainVision").get<bool>())
            {
                revia::core::SettingsFileEditor editor(overlay, settings.string(), profiles.string());
                std::string error;
                Require(
                    editor.SetBool(VisionKey, true, error), error.empty() ? "The supported settings writer refused the change." : error);
            }
            const auto after = Capture(settings, profiles, overlay);
            auto expectedValues = before.ownerValues;
            if (!before.report.at("effectiveMainVision").get<bool>())
                expectedValues[VisionKey] = true;
            Require(after.ownerValues == expectedValues && after.report.at("effectiveMainVision").get<bool>(),
                "The effective value or another owner choice did not match the requested single change.");
            for (const char* field : {"settingsSha256", "profileSha256", "otherOwnerChoicesSha256", "modelPath", "projectorPath",
                     "effectiveImageMinTokens", "contextSize", "configuredMaxTokens", "configuredThinkingTokens", "effectiveTemperature",
                     "screenVision", "continuousScreenAwareness"})
                Require(after.report.at(field) == before.report.at(field), "An unrelated setting or profile field changed.");
            report["after"] = after.report;
            report["otherOwnerChoicesPreserved"] = true;
        }
        else
            report["wouldChangeOwnerOverlay"] = !before.report.at("effectiveMainVision").get<bool>();
        std::ofstream output(receipt, std::ios::binary);
        output << report.dump(2);
        Require(output.good(), "The new settings receipt could not be retained.");
        std::cout << report.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
