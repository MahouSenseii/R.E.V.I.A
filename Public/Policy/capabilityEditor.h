#pragma once

#include <filesystem>
#include <string>

namespace revia::policy
{

// The only writer for capabilities.json. It edits the existing JSON instead of
// serializing runtime-normalized paths, preserving portable values such as %USERPROFILE%.
// Every candidate file is parsed by PermissionStore before it replaces the live config.
class CapabilityEditor
{
public:
    [[nodiscard]] bool AddApplication(
        const std::filesystem::path& path,
        const std::string& executable,
        std::string& outError) const;
    [[nodiscard]] bool RemoveApplication(
        const std::filesystem::path& path,
        const std::string& executable,
        std::string& outError) const;
    [[nodiscard]] bool AddControl(
        const std::filesystem::path& path,
        const std::string& executable,
        const std::string& control,
        std::string& outError) const;
    [[nodiscard]] bool RemoveControl(
        const std::filesystem::path& path,
        const std::string& executable,
        const std::string& control,
        std::string& outError) const;
    [[nodiscard]] bool SetInternetAccess(
        const std::filesystem::path& path,
        bool enabled,
        bool automaticLookup,
        std::string& outError) const;
    [[nodiscard]] bool SetInternetBrowser(
        const std::filesystem::path& path,
        bool visibleBrowser,
        bool autonomousResearch,
        std::string& outError) const;

private:
    enum class Mutation
    {
        AddApplication,
        RemoveApplication,
        AddControl,
        RemoveControl,
        Internet,
        Browser
    };

    [[nodiscard]] bool Apply(
        const std::filesystem::path& path,
        Mutation mutation,
        const std::string& executable,
        const std::string& control,
        bool enabled,
        bool automaticLookup,
        std::string& outError) const;
};

} // namespace revia::policy
