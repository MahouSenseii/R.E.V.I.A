#include "Perception/microphoneUse.h"

#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::perception
{

namespace
{

bool SameName(const std::string& left, const std::string& right)
{
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
        [](const unsigned char a, const unsigned char b)
        {
            return std::tolower(a) == std::tolower(b);
        });
}

#ifdef _WIN32
std::string ToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string converted(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        converted.data(), size, nullptr, nullptr);
    return converted;
}

std::vector<std::wstring> Subkeys(HKEY key)
{
    std::vector<std::wstring> names;
    for (DWORD index = 0;; ++index)
    {
        wchar_t name[512];
        DWORD length = 512;
        if (RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
        {
            break;
        }
        names.emplace_back(name, length);
    }
    return names;
}

// Windows sets LastUsedTimeStop to zero while an app holds the microphone.
bool InUse(HKEY parent, const std::wstring& name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(parent, name.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return false;
    }
    const auto read = [key](const wchar_t* value)
    {
        ULONGLONG data = 0;
        DWORD size = sizeof(data);
        DWORD type = 0;
        const bool found = RegQueryValueExW(key, value, nullptr, &type,
            reinterpret_cast<LPBYTE>(&data), &size) == ERROR_SUCCESS && type == REG_QWORD;
        return found ? data : 0ULL;
    };
    const bool active = read(L"LastUsedTimeStart") != 0 && read(L"LastUsedTimeStop") == 0;
    RegCloseKey(key);
    return active;
}
#endif

} // namespace

std::vector<MicrophoneUse> ReadMicrophoneUse()
{
    std::vector<MicrophoneUse> uses;
#ifdef _WIN32
    HKEY microphone = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\"
            L"ConsentStore\\microphone",
            0, KEY_READ, &microphone) != ERROR_SUCCESS)
    {
        return uses;
    }
    for (const std::wstring& name : Subkeys(microphone))
    {
        if (name != L"NonPackaged")
        {
            uses.push_back({ToUtf8(name), InUse(microphone, name)});
            continue;
        }
        HKEY desktopApps = nullptr;
        if (RegOpenKeyExW(microphone, name.c_str(), 0, KEY_READ, &desktopApps) != ERROR_SUCCESS)
        {
            continue;
        }
        // Desktop apps are keyed by their full path with '#' for each separator.
        for (const std::wstring& path : Subkeys(desktopApps))
        {
            const std::size_t separator = path.find_last_of(L'#');
            uses.push_back({ToUtf8(separator == std::wstring::npos ? path : path.substr(separator + 1)),
                InUse(desktopApps, path)});
        }
        RegCloseKey(desktopApps);
    }
    RegCloseKey(microphone);
#endif
    return uses;
}

bool OtherAppUsingMicrophone(
    const std::vector<MicrophoneUse>& uses,
    const std::vector<std::string>& ownApplications)
{
    return std::any_of(uses.begin(), uses.end(), [&ownApplications](const MicrophoneUse& use)
    {
        return use.active && std::none_of(ownApplications.begin(), ownApplications.end(),
            [&use](const std::string& own) { return SameName(use.application, own); });
    });
}

bool InCall()
{
    std::vector<std::string> own = {"ReviaDesktop.exe", "R_E_V_I_A.exe"};
#ifdef _WIN32
    wchar_t module[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
    {
        const std::wstring path(module, length);
        const std::size_t separator = path.find_last_of(L"\\/");
        own.push_back(ToUtf8(separator == std::wstring::npos ? path : path.substr(separator + 1)));
    }
#endif
    return OtherAppUsingMicrophone(ReadMicrophoneUse(), own);
}

} // namespace revia::perception
