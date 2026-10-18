#pragma once

// Windows pieces shared by every child process Revia owns directly: a closing handle,
// argument quoting that CommandLineToArgvW reverses exactly, and an environment block
// that starts from nothing. Both the one-shot process executor and the long-lived
// owned process use them, so a child is never quoted or given an environment two ways.

#ifdef _WIN32

#include "Actions/actionTypes.h"

#include <array>
#include <map>
#include <string>
#include <vector>

#include <windows.h>

namespace revia::process::windows
{

class Handle
{
  public:
    explicit Handle(HANDLE value = nullptr) : value(value)
    {
    }
    ~Handle()
    {
        Reset();
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HANDLE Get() const
    {
        return value;
    }
    [[nodiscard]] bool Valid() const
    {
        return value && value != INVALID_HANDLE_VALUE;
    }
    void Reset(HANDLE next = nullptr)
    {
        if (Valid())
            CloseHandle(value);
        value = next;
    }
    [[nodiscard]] HANDLE Release()
    {
        HANDLE owned = value;
        value = nullptr;
        return owned;
    }

  private:
    HANDLE value;
};

inline std::wstring Wide(const std::string& value)
{
    return actions::Utf8ToPath(value).native();
}

inline std::wstring Quote(const std::wstring& value)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : value)
    {
        if (character == L'\\')
        {
            ++slashes;
            continue;
        }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

struct EnvironmentOrder
{
    bool operator()(const std::wstring& left, const std::wstring& right) const
    {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    }
};

// Starts from the minimum Windows runtime environment, never the parent's secrets.
inline std::vector<wchar_t> MinimalEnvironment(const std::map<std::string, std::string>& environment)
{
    std::map<std::wstring, std::wstring, EnvironmentOrder> values;
    std::array<wchar_t, MAX_PATH + 1> windows{};
    const auto count = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    if (count > 0 && count < windows.size())
    {
        values[L"SystemRoot"] = windows.data();
        values[L"WINDIR"] = windows.data();
    }
    for (const auto& [name, value] : environment)
        values[Wide(name)] = Wide(value);
    std::vector<wchar_t> block;
    for (const auto& [name, value] : values)
    {
        const auto entry = name + L"=" + value;
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (block.size() == 1)
        block.push_back(L'\0');
    return block;
}

// A pipe whose `parentEnd` is never inherited; the other end is, through the explicit
// handle list only.
inline bool Pipe(Handle& reader, Handle& writer, const bool parentReads)
{
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!CreatePipe(&read, &write, &security, 64U * 1024U))
        return false;
    reader.Reset(read);
    writer.Reset(write);
    return SetHandleInformation(parentReads ? read : write, HANDLE_FLAG_INHERIT, 0) != FALSE;
}

} // namespace revia::process::windows

#endif
