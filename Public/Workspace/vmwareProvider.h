#pragma once

#include "Workspace/virtualMachineRuntime.h"

#include <filesystem>
#include <memory>
#include <string>

namespace revia::workspace
{
struct VmwareProviderOptions
{
    std::filesystem::path root;
    std::string ownerId;
    std::filesystem::path pythonExecutable;
    std::filesystem::path bootstrapScript;
};

// Fixed trusted lifecycle bridge. Model proposals never choose host paths or commands.
[[nodiscard]] std::shared_ptr<IVirtualMachineProvider> CreateVmwareProvider(VmwareProviderOptions options);
}
