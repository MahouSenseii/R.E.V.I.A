#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace revia::runtime
{
struct CompanionDescriptor
{
    std::string id;
    std::string displayName;
    std::string profileId;
    bool legacy = false;
};

class CompanionPaths
{
public:
    CompanionPaths(std::filesystem::path absoluteInstallRoot, CompanionDescriptor descriptor);
    [[nodiscard]] const std::filesystem::path& InstallRoot() const { return installRoot; }
    [[nodiscard]] const std::filesystem::path& Root() const { return root; }
    [[nodiscard]] const CompanionDescriptor& Descriptor() const { return descriptor; }
    [[nodiscard]] std::filesystem::path Resolve(const std::filesystem::path& relative) const;

private:
    std::filesystem::path installRoot;
    std::filesystem::path root;
    CompanionDescriptor descriptor;
};

class CompanionRegistry
{
public:
    explicit CompanionRegistry(std::filesystem::path absoluteInstallRoot);
    ~CompanionRegistry();
    CompanionRegistry(const CompanionRegistry&) = delete;
    CompanionRegistry& operator=(const CompanionRegistry&) = delete;
    bool Load(std::string& outError);
    [[nodiscard]] std::vector<CompanionDescriptor> List() const;
    [[nodiscard]] std::optional<CompanionDescriptor> Find(const std::string& id) const;
    [[nodiscard]] std::string SelectedId() const;
    bool Create(std::string displayName, std::string profileId, CompanionDescriptor& outCompanion, std::string& outError);
    bool Select(const std::string& id, std::string& outError);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

enum class CompanionMigrationPhase { Inventory, Copy, Validate, Publish };
using CompanionMigrationHook = std::function<bool(CompanionMigrationPhase, const std::filesystem::path&)>;
struct CompanionMigrationResult
{
    bool complete = false;
    bool interrupted = false;
    std::size_t copiedArtifacts = 0;
    std::string error;
};

[[nodiscard]] std::vector<std::filesystem::path> InventoryLegacyCompanion(const CompanionPaths& source);
// The caller quiesces source writers. Staging survives interruption; publication never replaces an occupied root.
[[nodiscard]] CompanionMigrationResult MigrateLegacyCompanion(const CompanionPaths& source, const CompanionPaths& target,
    CompanionMigrationHook hook = {});
}
