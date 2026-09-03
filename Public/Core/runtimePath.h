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

// The directory relative runtime paths are anchored to.
//
// The launch directory when it looks like a Revia runtime root, otherwise the
// executable's directory or the nearest ancestor of it that does. "Looks like" means it
// holds a Config directory, which is the layout every install shape shares.
[[nodiscard]] std::filesystem::path RuntimeRoot();

// ResolveRuntimePath for a path that does not exist yet.
//
// The read-side resolver returns an ancestor candidate only when it already exists,
// which is right for locating an installed model and wrong for deciding where to create
// something. On a first run, or after a directory is cleared, that falls through to
// std::filesystem::absolute -- which is relative to the process working directory. A
// shortcut, a startup entry, or a launcher with a different working directory therefore
// created a second RuntimeData tree somewhere unintended.
//
// The rules are the same; only the existence requirement differs. An absolute
// configured path is returned unchanged.
[[nodiscard]] std::filesystem::path ResolveRuntimeWritePath(
    const std::filesystem::path& configuredPath);

[[nodiscard]] inline std::filesystem::path ResolveRuntimeWritePath(
    const std::string& configuredPath)
{
    return ResolveRuntimeWritePath(std::filesystem::path(configuredPath));
}

} // namespace revia::core
