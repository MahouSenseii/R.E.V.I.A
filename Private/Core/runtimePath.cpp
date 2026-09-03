#include "Core/runtimePath.h"

#include <array>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::core
{

namespace
{

std::filesystem::path ExecutableDirectory()
{
#ifdef _WIN32
    std::array<wchar_t, 32768> module{};
    const DWORD length = GetModuleFileNameW(
        nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (length > 0 && length < module.size())
    {
        return std::filesystem::path(
            std::wstring(module.data(), length)).parent_path();
    }
#endif
    std::error_code error;
    return std::filesystem::current_path(error);
}

std::filesystem::path ExistingFromAncestors(
    std::filesystem::path root,
    const std::filesystem::path& relative)
{
    for (int depth = 0; depth < 6 && !root.empty(); ++depth)
    {
        const std::filesystem::path candidate = (root / relative).lexically_normal();
        std::error_code error;
        if (std::filesystem::exists(candidate, error) && !error)
        {
            return candidate;
        }
        const std::filesystem::path parent = root.parent_path();
        if (parent == root)
        {
            break;
        }
        root = parent;
    }
    return {};
}

// A directory that holds a Config folder is a Revia runtime root. Every install shape
// -- development build output, packaged copy, portable directory -- has one, and
// nothing else Revia might be launched from does.
bool LooksLikeRuntimeRoot(const std::filesystem::path& candidate)
{
    std::error_code error;
    return std::filesystem::is_directory(candidate / "Config", error) && !error;
}

std::filesystem::path NearestRuntimeRoot(std::filesystem::path root)
{
    for (int depth = 0; depth < 6 && !root.empty(); ++depth)
    {
        if (LooksLikeRuntimeRoot(root)) return root;
        const std::filesystem::path parent = root.parent_path();
        if (parent == root) break;
        root = parent;
    }
    return {};
}

} // namespace

std::filesystem::path RuntimeRoot()
{
    // The launch directory wins when it is itself a runtime root, so running from the
    // build output behaves exactly as it did before this existed.
    std::error_code error;
    const std::filesystem::path launchDirectory =
        std::filesystem::current_path(error);
    if (!error)
    {
        if (const std::filesystem::path fromLaunch =
                NearestRuntimeRoot(launchDirectory);
            !fromLaunch.empty())
        {
            return fromLaunch;
        }
    }
    const std::filesystem::path executable = ExecutableDirectory();
    if (const std::filesystem::path fromExecutable = NearestRuntimeRoot(executable);
        !fromExecutable.empty())
    {
        return fromExecutable;
    }
    return executable;
}

std::filesystem::path ResolveRuntimeWritePath(
    const std::filesystem::path& configuredPath)
{
    if (configuredPath.empty() || configuredPath.is_absolute())
    {
        return configuredPath.lexically_normal();
    }
    // Anchored only to the canonical runtime root, never to an existing file found by
    // searching the working directory (or the executable directory) and its ancestors.
    // That search is right for locating an installed, read-only artifact, but for a
    // write target it let an old accidental RuntimeData tree -- created before
    // RuntimeRoot() existed, or by a launcher with a different working directory -- go
    // on hijacking Presence forever, because the stale file it finds always "already
    // exists". A write target has exactly one right answer, and RuntimeRoot() already
    // is it.
    return (RuntimeRoot() / configuredPath).lexically_normal();
}

std::filesystem::path ResolveRuntimePath(
    const std::filesystem::path& configuredPath)
{
    if (configuredPath.empty() || configuredPath.is_absolute())
    {
        return configuredPath.lexically_normal();
    }

    std::error_code error;
    const std::filesystem::path launchDirectory =
        std::filesystem::current_path(error);
    if (!error)
    {
        const std::filesystem::path resolved =
            ExistingFromAncestors(launchDirectory, configuredPath);
        if (!resolved.empty())
        {
            return resolved;
        }
    }

    const std::filesystem::path resolved =
        ExistingFromAncestors(ExecutableDirectory(), configuredPath);
    if (!resolved.empty())
    {
        return resolved;
    }

    error.clear();
    const std::filesystem::path absolute =
        std::filesystem::absolute(configuredPath, error);
    return error ? configuredPath.lexically_normal() : absolute.lexically_normal();
}

} // namespace revia::core
