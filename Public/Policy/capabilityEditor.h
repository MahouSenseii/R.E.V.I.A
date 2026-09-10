#pragma once

#include "Actions/actionTypes.h"

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
    // enabled grants "may look when asked"; autonomousCapture additionally grants "may
    // decide to look". Revoking the first revokes the second, because a subset authority
    // cannot outlive the authority it is a subset of.
    [[nodiscard]] bool SetCameraAccess(
        const std::filesystem::path& path,
        bool enabled,
        bool autonomousCapture,
        std::string& outError) const;
    // Withdrawing pointer control withdraws raw coordinates with it, and withdrawing
    // every hand withdraws autonomy, for the same reason the camera works that way:
    // a subset authority must not survive the authority it is a subset of and quietly
    // return when that one is granted again.
    [[nodiscard]] bool SetDesktopControl(
        const std::filesystem::path& path,
        bool pointer,
        bool keyboard,
        bool applicationLaunch,
        bool rawCoordinates,
        bool autonomous,
        actions::CapabilitySettings::DesktopControl::InputScope scope,
        bool allowCommandSurfaces,
        std::string& outError) const;
    // Execution mode is the owner's choice of how much stops to ask. It never changes
    // which roots, applications, or controls are in scope.
    [[nodiscard]] bool SetExecutionMode(
        const std::filesystem::path& path,
        actions::ExecutionMode mode,
        std::string& outError) const;

private:
    enum class Mutation
    {
        AddApplication,
        RemoveApplication,
        AddControl,
        RemoveControl,
        Internet,
        Browser,
        Camera,
        DesktopControl,
        Mode
    };

    struct DesktopControlChange
    {
        bool pointer = false;
        bool keyboard = false;
        bool applicationLaunch = false;
        bool rawCoordinates = false;
        bool autonomous = false;
        actions::CapabilitySettings::DesktopControl::InputScope scope =
            actions::CapabilitySettings::DesktopControl::InputScope::ApprovedApplications;
        bool allowCommandSurfaces = false;
    };

    [[nodiscard]] bool Apply(
        const std::filesystem::path& path,
        Mutation mutation,
        const std::string& executable,
        const std::string& control,
        bool enabled,
        bool automaticLookup,
        std::string& outError,
        // Passed explicitly by every caller: a nested aggregate cannot supply a default
        // argument from inside the class that owns it.
        const DesktopControlChange& desktop,
        actions::ExecutionMode mode) const;
};

} // namespace revia::policy
