#include "Windows/applicationLocator.h"

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
    const DWORD length = SearchPathW(nullptr, name.c_str(), nullptr,
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
