#pragma once

#include "Actions/actionTypes.h"

#include <filesystem>
#include <string>

namespace revia::policy
{

class PermissionStore
{
public:
    [[nodiscard]] bool Load(
        const std::filesystem::path& path,
        actions::CapabilitySettings& outSettings,
        std::string& outError) const;

    [[nodiscard]] static std::string ExpandEnvironmentVariables(const std::string& value);
};

} // namespace revia::policy
