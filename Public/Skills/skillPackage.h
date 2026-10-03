#pragma once

#include "Actions/actionTypes.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::skills
{

struct SkillPackageReference
{
    std::string id;
    std::string version;
    std::string digest;
};

struct SkillManifest
{
    std::uint32_t schemaVersion = 1;
    std::string id;
    std::string version;
    std::vector<std::string> supportedTasks;
    std::string inputSchema;
    std::string outputSchema;
    std::vector<actions::ActionType> requiredCapabilities;
    std::vector<actions::ActionType> optionalCapabilities;
    std::vector<std::string> sideEffects;
    std::map<std::string, std::string> supportedToolVersions;
    std::string verificationEntry = "cases.json";
    std::string outputFormat = "counts";
};

struct SkillCase
{
    std::string id;
    std::vector<std::string> entries;
    std::string expectedSummary;
};

struct SkillPackage
{
    SkillPackageReference reference;
    SkillManifest manifest;
    std::string procedure;
    std::vector<SkillCase> cases;
};

// A copied package is the task's procedure. Activation changes never rewrite it.
using SkillPin = SkillPackage;

struct SkillInventoryResult
{
    bool verified = false;
    bool complete = false;
    std::size_t entries = 0;
    std::size_t files = 0;
    std::size_t directories = 0;
    std::size_t links = 0;
    std::size_t other = 0;
    std::string summary;
};

struct SkillVerification
{
    std::string packageDigest;
    std::string evidenceDigest;
    std::size_t regressionCases = 0;
    std::size_t freshCases = 0;
    bool passed = false;
    std::vector<SkillCase> freshEvidence;
};

struct SkillExportReview
{
    std::string packageDigest;
    std::string evidenceDigest;
    bool syntheticExamplesReviewed = false;
    bool disclosureReviewed = false;
    bool neutralProcedureApproved = false;
};

[[nodiscard]] bool LoadSkillPackage(const std::filesystem::path& directory, SkillPackage& outPackage, std::string& outError);
[[nodiscard]] bool RequiresCapability(const SkillPin& pin, actions::ActionType operation);
[[nodiscard]] SkillInventoryResult SummarizeInventory(const SkillPin& pin, const std::vector<std::string>& admittedEntries);
[[nodiscard]] SkillVerification VerifySkillPackage(const SkillPackage& package, const std::vector<SkillCase>& freshCases);

// Immutable package artifacts and companion-private activation/export decisions; no code execution or authority grants.
class SkillPackageStore
{
public:
    SkillPackageStore(std::filesystem::path privateDirectory, std::filesystem::path neutralSeedDirectory);
    bool Initialize(std::string& outError);
    bool Publish(const SkillPackage& package, const SkillVerification& verification, const SkillExportReview& review, std::string& outError);
    bool Activate(const SkillPackageReference& reference, std::string& outError);
    bool Rollback(const std::string& id, std::string& outError);
    [[nodiscard]] std::optional<SkillPin> Pin(const std::string& id) const;
    [[nodiscard]] std::optional<SkillPin> LoadPinned(const SkillPackageReference& reference) const;
    bool Export(const SkillPackageReference& reference, const std::filesystem::path& destination, std::string& outError) const;

private:
    bool SaveState(std::string& outError) const;
    mutable std::mutex mutex;
    std::filesystem::path directory;
    std::filesystem::path seedDirectory;
    std::map<std::string, SkillPackage> packages;
    std::map<std::string, std::string> active;
    std::map<std::string, std::vector<std::string>> previous;
    std::map<std::string, SkillExportReview> exportReviews;
    bool initialized = false;
};

} // namespace revia::skills
