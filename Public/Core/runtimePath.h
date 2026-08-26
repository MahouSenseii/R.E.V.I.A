#pragma once

#include <filesystem>
#include <string>

namespace revia::core
{

// Resolves a configured runtime artifact from either the launch directory or the
// executable's directory and its parents. This keeps development builds, installed
// layouts, and portable copies using the same relative settings paths.
[[nodiscard]] std::filesystem::path ResolveRuntimePath(
    const std::filesystem::path& configuredPath);

[[nodiscard]] inline std::filesystem::path ResolveRuntimePath(
    const std::string& configuredPath)
{
    return ResolveRuntimePath(std::filesystem::path(configuredPath));
}

} // namespace revia::core
