#include "Windows/applicationLocator.h"

#include <algorithm>
#include <vector>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::actions::windows
{

std::wstring ResolveApplicationExecutable(const std::wstring& name)
{
#ifdef _WIN32
    if (name.empty() || name.find_first_of(L"/\\:\"\r\n") != std::wstring::npos ||
        name.find(L'\0') != std::wstring::npos || name.size() < 5 ||
        _wcsicmp(name.substr(name.size() - 4).c_str(), L".exe") != 0)
    {
        return {};
    }
    std::vector<wchar_t> buffer(32768);
    // An explicit search path: the system folders, then each absolute folder on PATH.
    // The default search also looks in the current directory, so a file of the same
    // name dropped wherever Revia happens to be working would be launched instead.
    std::wstring searchPath;
    const auto addFolder = [&searchPath](const std::wstring& folder)
    {
        if (folder.empty() || folder.find(L'"') != std::wstring::npos || !std::filesystem::path(folder).is_absolute())
            return;
        if (!searchPath.empty())
            searchPath.push_back(L';');
        searchPath += folder;
    };
    if (const UINT systemLength = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
        systemLength > 0 && systemLength < buffer.size())
        addFolder(std::wstring(buffer.data(), systemLength));
    if (const UINT windowsLength = GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
        windowsLength > 0 && windowsLength < buffer.size())
        addFolder(std::wstring(buffer.data(), windowsLength));
    if (const DWORD pathLength = GetEnvironmentVariableW(L"PATH", buffer.data(), static_cast<DWORD>(buffer.size()));
        pathLength > 0 && pathLength < buffer.size())
    {
        const std::wstring path(buffer.data(), pathLength);
        for (std::size_t start = 0; start <= path.size();)
        {
            const std::size_t end = std::min(path.find(L';', start), path.size());
            addFolder(path.substr(start, end - start));
            start = end + 1;
        }
    }
    const DWORD length = searchPath.empty() ? 0 : SearchPathW(searchPath.c_str(), name.c_str(), nullptr,
        static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length > 0 && length < buffer.size())
    {
        return {buffer.data(), length};
    }

    // Browsers normally register here without adding their install directory to PATH.
    // Read only the executable value, never the optional command/search-path fields.
    const std::wstring key =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + name;
    for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
    {
        for (const REGSAM view : {REGSAM{KEY_WOW64_64KEY}, REGSAM{KEY_WOW64_32KEY}})
        {
            HKEY registration = nullptr;
            if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE | view,
                &registration) != ERROR_SUCCESS)
            {
                continue;
            }
            DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
            const LSTATUS status = RegGetValueW(registration, nullptr, nullptr,
                RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buffer.data(), &bytes);
            RegCloseKey(registration);
            if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t))
            {
                continue;
            }
            std::wstring value(buffer.data(), bytes / sizeof(wchar_t) - 1);
            if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
            {
                value = value.substr(1, value.size() - 2);
            }
            if (value.find(L'\0') != std::wstring::npos ||
                value.find(L'"') != std::wstring::npos || value.starts_with(L"\\\\"))
            {
                continue;
            }
            const std::filesystem::path candidate(value);
            std::error_code error;
            if (candidate.is_absolute() &&
                _wcsicmp(candidate.filename().c_str(), name.c_str()) == 0 &&
                std::filesystem::is_regular_file(candidate, error))
            {
                return candidate.wstring();
            }
        }
    }
#else
    (void)name;
#endif
    return {};
}

} // namespace revia::actions::windows
