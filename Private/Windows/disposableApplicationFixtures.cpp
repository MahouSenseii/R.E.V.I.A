#include "Windows/disposableApplicationFixtures.h"
#include "Goals/goalSandbox.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <uiautomation.h>
#endif

namespace revia::actions::windows
{

namespace
{
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

#ifdef _WIN32
template <typename T>
void Release(T*& value)
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}

std::wstring ProcessName(const DWORD processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) return {};
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const bool read = QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process);
    if (!read) return {};
    path.resize(length);
    std::wstring name = std::filesystem::path(path).filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    return name;
}

struct WindowList
{
    std::wstring executable;
    std::vector<HWND> windows;
};

BOOL CALLBACK CollectWindows(const HWND window, const LPARAM parameter)
{
    if (!IsWindowVisible(window)) return TRUE;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    auto& list = *reinterpret_cast<WindowList*>(parameter);
    if (ProcessName(processId) == list.executable)
    {
        list.windows.push_back(window);
    }
    return TRUE;
}

std::vector<HWND> WindowsFor(const std::wstring& executable)
{
    WindowList list{executable, {}};
    EnumWindows(CollectWindows, reinterpret_cast<LPARAM>(&list));
    return list.windows;
}

std::set<DWORD> ProcessIdsFor(const std::wstring& executable)
{
    std::set<DWORD> ids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return ids;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            std::wstring name = entry.szExeFile;
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name == executable) ids.insert(entry.th32ProcessID);
        }
        while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return ids;
}

std::string WindowTitle(const HWND window)
{
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};
    std::wstring value(static_cast<std::size_t>(length + 1), L'\0');
    const int copied = GetWindowTextW(window, value.data(), length + 1);
    if (copied <= 0) return {};
    value.resize(static_cast<std::size_t>(copied));
    const int bytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), copied, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string output(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), copied,
        output.data(), bytes, nullptr, nullptr);
    return output;
}

std::string StableWindowTitle(const HWND window)
{
    std::string previous;
    int stableSamples = 0;
    for (int attempt = 0; attempt < 60; ++attempt)
    {
        if (!IsWindow(window)) return {};
        const std::string current = WindowTitle(window);
        if (!current.empty() && current == previous)
        {
            ++stableSamples;
            if (stableSamples >= 5) return current;
        }
        else
        {
            previous = current;
            stableSamples = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return previous;
}

HANDLE LaunchProcess(std::wstring command)
{
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const bool launched = CreateProcessW(
        nullptr, command.data(), nullptr, nullptr, FALSE, 0,
        nullptr, nullptr, &startup, &process) != FALSE;
    if (!launched)
    {
        return nullptr;
    }
    CloseHandle(process.hThread);
    return process.hProcess;
}

bool IsIsolatedNotepadWindow(const HWND window)
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = initialized == S_OK || initialized == S_FALSE;
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return false;
    IUIAutomation* automation = nullptr;
    IUIAutomationElement* root = nullptr;
    IUIAutomationCondition* condition = nullptr;
    IUIAutomationElementArray* tabs = nullptr;
    bool isolated = false;
    if (SUCCEEDED(CoCreateInstance(
            CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
            reinterpret_cast<void**>(&automation))) && automation != nullptr &&
        SUCCEEDED(automation->ElementFromHandle(window, &root)) && root != nullptr)
    {
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_I4;
        value.lVal = UIA_TabItemControlTypeId;
        if (SUCCEEDED(automation->CreatePropertyCondition(
                UIA_ControlTypePropertyId, value, &condition)) && condition != nullptr &&
            SUCCEEDED(root->FindAll(TreeScope_Descendants, condition, &tabs)) &&
            tabs != nullptr)
        {
            int count = 0;
            isolated = SUCCEEDED(tabs->get_Length(&count)) && count <= 1;
        }
        VariantClear(&value);
    }
    Release(tabs);
    Release(condition);
    Release(root);
    Release(automation);
    if (shouldUninitialize) CoUninitialize();
    return isolated;
}

#endif
}

DisposableApplicationFixtures::~DisposableApplicationFixtures()
{
    Close();
}

bool DisposableApplicationFixtures::Launch(
    const std::vector<std::string>& applications,
    const std::filesystem::path& scratchRoot,
    std::string& outError)
{
    Close();
#ifdef _WIN32
    for (const std::string& requested : applications)
    {
        const std::string application = Lower(requested);
        const std::wstring executable(application.begin(), application.end());
        const std::vector<HWND> before = WindowsFor(executable);
        const std::set<DWORD> beforeProcessIds = ProcessIdsFor(executable);
        HANDLE launchedProcess = nullptr;
        if (application == "notepad.exe")
        {
            std::filesystem::path document = scratchRoot / "NotepadFixture.txt";
            std::error_code error;
            std::filesystem::create_directories(scratchRoot, error);
            std::ofstream seed(document, std::ios::binary | std::ios::trunc);
            if (error || !seed)
            {
                outError = "Could not create the disposable Notepad document.";
                Close();
                return false;
            }
            seed.close();
            std::wstring command = L"C:\\Windows\\System32\\notepad.exe \"" +
                document.wstring() + L"\"";
            launchedProcess = LaunchProcess(std::move(command));
        }
        else if (application == "explorer.exe")
        {
            std::filesystem::path folder = scratchRoot / "ExplorerFixture";
            std::error_code error;
            std::filesystem::create_directories(folder, error);
            if (error)
            {
                outError = "Could not create the disposable Explorer folder.";
                Close();
                return false;
            }
            std::wstring command = L"C:\\Windows\\explorer.exe /separate,\"" +
                folder.wstring() + L"\"";
            launchedProcess = LaunchProcess(std::move(command));
        }
        else
        {
            outError = "No disposable fixture exists for " + application + ".";
            Close();
            return false;
        }
        if (launchedProcess == nullptr)
        {
            outError = "Windows could not launch the disposable " + application + " fixture.";
            Close();
            return false;
        }

        HWND created = nullptr;
        for (int attempt = 0; attempt < 100 && created == nullptr; ++attempt)
        {
            for (const HWND candidate : WindowsFor(executable))
            {
                if (std::find(before.begin(), before.end(), candidate) == before.end())
                {
                    created = candidate;
                    break;
                }
            }
            if (created == nullptr) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (created == nullptr)
        {
            CloseHandle(launchedProcess);
            outError = application +
                " did not create a distinct window; rehearsal will not use a user window.";
            Close();
            return false;
        }
        if (application == "notepad.exe")
        {
            if (!IsIsolatedNotepadWindow(created))
            {
                PostMessageW(created, WM_CLOSE, 0, 0);
                CloseHandle(launchedProcess);
                outError = "Notepad restored user tabs instead of creating an isolated fixture.";
                Close();
                return false;
            }
        }
        const std::string title = StableWindowTitle(created);
        if (title.empty())
        {
            PostMessageW(created, WM_CLOSE, 0, 0);
            CloseHandle(launchedProcess);
            outError = "The disposable " + application + " window had no stable title.";
            Close();
            return false;
        }
        if (application == "explorer.exe")
        {
            DWORD windowProcessId = 0;
            GetWindowThreadProcessId(created, &windowProcessId);
            if (windowProcessId == 0 || beforeProcessIds.contains(windowProcessId))
            {
                PostMessageW(created, WM_CLOSE, 0, 0);
                CloseHandle(launchedProcess);
                outError = "Explorer did not create a separate disposable process.";
                Close();
                return false;
            }
            HANDLE windowProcess = OpenProcess(
                SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                windowProcessId);
            if (windowProcess == nullptr)
            {
                PostMessageW(created, WM_CLOSE, 0, 0);
                CloseHandle(launchedProcess);
                outError = "The disposable Explorer process could not be retained.";
                Close();
                return false;
            }
            CloseHandle(launchedProcess);
            launchedProcess = windowProcess;
        }
        fixtures[application] = {
            title,
            reinterpret_cast<std::uintptr_t>(created),
            reinterpret_cast<std::uintptr_t>(launchedProcess)};
    }
    outError.clear();
    return true;
#else
    (void)applications;
    (void)scratchRoot;
    outError = "Disposable UI Automation fixtures are currently implemented for Windows.";
    return false;
#endif
}

bool DisposableApplicationFixtures::Retarget(goals::Goal& goal, std::string& outError) const
{
    for (goals::GoalStep& step : goal.steps)
    {
        for (actions::ActionRequest* request : {&step.action, &step.check})
        {
            if (!goals::GoalSandbox::IsDesktopAction(request->type))
            {
                continue;
            }
            const auto fixture = fixtures.find(Lower(request->application));
            if (fixture == fixtures.end())
            {
                outError = "A desktop rehearsal step has no disposable window.";
                return false;
            }
            request->windowTitle = fixture->second.windowTitle;
        }
    }
    outError.clear();
    return true;
}

void DisposableApplicationFixtures::Close()
{
#ifdef _WIN32
    for (const auto& [application, fixture] : fixtures)
    {
        (void)application;
        const HWND window = reinterpret_cast<HWND>(fixture.windowHandle);
        if (window != nullptr && IsWindow(window))
        {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
    }
    for (const auto& [application, fixture] : fixtures)
    {
        (void)application;
        const HANDLE process = reinterpret_cast<HANDLE>(fixture.processHandle);
        if (process == nullptr) continue;
        if (WaitForSingleObject(process, 1500) == WAIT_TIMEOUT &&
            application == "explorer.exe")
        {
            // This is the exact handle returned by CreateProcess for the disposable
            // instance, never a PID looked up after the fact.
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 500);
        }
        CloseHandle(process);
    }
#endif
    fixtures.clear();
}

} // namespace revia::actions::windows
