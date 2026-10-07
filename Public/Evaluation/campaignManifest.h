#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace revia::evaluation
{

struct CampaignManifest
{
    std::uint32_t schemaVersion = 1;
    std::string campaignId;
    std::string commit;
    std::string sourceDigest;
    bool sourceDirty = false;
    std::string buildDigest;
    std::string providerDigest;
    bool providerAvailable = false;
    std::string settingsDigest;
    std::string fixtureDigest;
    std::string oracleVersion;
    std::string hardware;
    std::uint64_t seed = 0;
    std::string timingBoundary;
};

[[nodiscard]] bool ValidateCampaignManifest(const CampaignManifest& manifest, std::string& error);
// Validate each manifest first. Equal identity does not establish available provider evidence or live qualification.
[[nodiscard]] std::vector<std::string> CampaignIdentityMismatches(const CampaignManifest& expected, const CampaignManifest& candidate);
[[nodiscard]] std::string SerializeCampaignManifest(const CampaignManifest& manifest);
[[nodiscard]] bool ParseCampaignManifest(const std::string& bytes, CampaignManifest& manifest, std::string& error);
[[nodiscard]] bool WriteCampaignManifestOnce(const std::filesystem::path& path, const CampaignManifest& manifest, std::string& error);
[[nodiscard]] bool WriteEvaluationArtifactOnce(const std::filesystem::path& path, const std::string& bytes, std::string& error);

}
