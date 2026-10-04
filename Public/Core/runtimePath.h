#pragma once

#include <filesystem>
#include <string>

namespace revia::core
{

// Resolves a configured runtime artifact from either the launch directory or the
// executable's directory and its parents. This keeps development builds, installed
// layouts, and portable copies using the same relative settings paths.
[[nodiscard]] std::filesystem::path ResolveRuntimePath(const std::filesystem::path& configuredPath);

[[nodiscard]] inline std::filesystem::path ResolveRuntimePath(const std::string& configuredPath)
{
    return ResolveRuntimePath(std::filesystem::path(configuredPath));
}

// Finds an existing read-only artifact (server binary, model, script) under root or the
// nearest ancestor of root that holds it. Build output directories hold Config but not
// ThirdParty or Models, which stay at the repository root above them. Returns empty
// when no candidate exists or relative is not a relative path.
[[nodiscard]] std::filesystem::path FindInstalledArtifact(const std::filesystem::path& root, const std::filesystem::path& relative);

// The directory relative runtime paths are anchored to.
//
// The launch directory when it looks like a Revia runtime root, otherwise the
// executable's directory or the nearest ancestor of it that does. "Looks like" means it
// holds a Config directory, which is the layout every install shape shares.
[[nodiscard]] std::filesystem::path RuntimeRoot();

// Resolve configured write paths without requiring the target to exist.
// Uses runtime ancestors instead of the working directory; absolute paths remain unchanged.
[[nodiscard]] std::filesystem::path ResolveRuntimeWritePath(const std::filesystem::path& configuredPath);

[[nodiscard]] inline std::filesystem::path ResolveRuntimeWritePath(const std::string& configuredPath)
{
    return ResolveRuntimeWritePath(std::filesystem::path(configuredPath));
}

// Finds the executable directory through Windows APIs, or argv[0] elsewhere.
// Returns empty when unavailable; Windows resolution supports Unicode and PATH launches.
[[nodiscard]] std::filesystem::path ProgramDirectory(const char* argumentZero);

} // namespace revia::core
