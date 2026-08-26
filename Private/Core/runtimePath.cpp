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

} // namespace

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
