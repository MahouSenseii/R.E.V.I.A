#include "Windows/applicationLocator.h"
#include "testSupport.h"

#include <fstream>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

void RunApplicationLocatorTests()
{
#ifdef _WIN32
    using revia::actions::windows::ResolveApplicationExecutable;
    using revia::tests::Check;
    const std::wstring name = L"revia-locator-fixture-" +
        std::to_wstring(GetCurrentProcessId()) + L".exe";
    const auto directory = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directory(directory);
    const auto executable = directory / name;
    { std::ofstream file(executable); file << "fixture only; never executed"; }
    const std::wstring key =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + name;
    HKEY registration = nullptr;
    DWORD disposition = 0;
    Check(RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &registration, &disposition) == ERROR_SUCCESS,
        "Could not create the disposable App Paths registration.");
    Check(disposition == REG_CREATED_NEW_KEY, "The fixture registry key already exists.");
    struct Cleanup
    {
        HKEY handle;
        std::wstring key;
        std::filesystem::path file;
        ~Cleanup()
        {
            RegCloseKey(handle);
            RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
            std::error_code error;
            std::filesystem::remove(file, error);
            std::filesystem::remove(file.parent_path(), error);
        }
    } cleanup{registration, key, executable};
    const auto setPath = [&](const std::wstring& value)
    {
        Check(RegSetValueExW(registration, nullptr, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS,
            "Could not write the fixture App Paths value.");
    };
    setPath(executable.wstring());
    Check(ResolveApplicationExecutable(name) == executable.wstring(),
        "An installed application registered outside PATH could not be found.");
    setPath(L"\"" + executable.wstring() + L"\"");
    Check(ResolveApplicationExecutable(name) == executable.wstring(),
        "A quoted registered executable could not be found.");
    setPath(executable.wstring() + L" --unexpected-argument");
    Check(ResolveApplicationExecutable(name).empty(),
        "An App Paths command line was accepted as an executable.");
    setPath((directory / L"missing.exe").wstring());
    Check(ResolveApplicationExecutable(name).empty(),
        "A stale registration was accepted.");
    Check(ResolveApplicationExecutable(executable.wstring()).empty() &&
        ResolveApplicationExecutable(L"msedge.exe --argument").empty(),
        "The executable lookup accepted a path or a command line.");
    Check(!ResolveApplicationExecutable(L"notepad.exe").empty(),
        "The existing Windows executable search stopped working.");
    std::cout << "Application lookup tests passed.\n";
#endif
}

#ifdef REVIA_LOCATOR_STANDALONE
int main()
{
    try { RunApplicationLocatorTests(); }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#endif
