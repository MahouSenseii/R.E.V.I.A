#pragma once

#include <filesystem>
#include <string>

namespace revia::workspace
{
struct VirtualMachineSettings
{
    bool enabled = false;
    std::string ownerId;
};

struct VirtualMachineSettingsResult
{
    bool succeeded = false;
    VirtualMachineSettings settings;
    std::string code;
    std::string message;
};

// Startup opt-in is companion-local authority, never a conversational preference.
// A missing file is disabled; invalid settings cannot enable a provider.
[[nodiscard]] VirtualMachineSettingsResult LoadVirtualMachineSettings(
    const std::filesystem::path& path, const std::string& expectedOwnerId);
}
