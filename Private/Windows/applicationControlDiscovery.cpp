#include "Windows/applicationControlDiscovery.h"

#include <algorithm>
#include <filesystem>
#include <set>

#ifdef _WIN32
#include <windows.h>
#include <uiautomation.h>
#endif

namespace revia::actions::windows
{

namespace
{
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

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string output(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        output.data(), count, nullptr, nullptr);
    return output;
}

std::string Property(
    IUIAutomationElement* element,
    HRESULT (STDMETHODCALLTYPE IUIAutomationElement::*getter)(BSTR*))
{
    BSTR value = nullptr;
    if (FAILED((element->*getter)(&value)) || value == nullptr) return {};
    const std::string output = WideToUtf8(std::wstring(value, SysStringLen(value)));
    SysFreeString(value);
    return output;
}

bool Supports(IUIAutomationElement* element, const PATTERNID patternId)
{
    IUnknown* pattern = nullptr;
    const bool result = SUCCEEDED(element->GetCurrentPattern(patternId, &pattern)) &&
        pattern != nullptr;
    Release(pattern);
    return result;
}

std::string ExecutableForWindow(const HWND window)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) return {};
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const bool read = QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process);
    if (!read) return {};
    path.resize(length);
    return WideToUtf8(std::filesystem::path(path).filename().wstring());
}
#endif
}

ApplicationControlInventory ApplicationControlDiscovery::InspectForeground(
    const int maxControls) const
{
    ApplicationControlInventory result;
#ifdef _WIN32
    const HWND window = GetForegroundWindow();
    if (window == nullptr || !IsWindowVisible(window) || IsIconic(window))
    {
        result.reason = "Windows did not report a visible foreground window.";
        return result;
    }
    result.application = ExecutableForWindow(window);
    const int titleLength = GetWindowTextLengthW(window);
    if (titleLength > 0)
    {
        std::wstring title(static_cast<std::size_t>(titleLength + 1), L'\0');
        const int copied = GetWindowTextW(window, title.data(), titleLength + 1);
        if (copied > 0)
        {
            title.resize(static_cast<std::size_t>(copied));
            result.windowTitle = WideToUtf8(title);
        }
    }
    if (result.application.empty())
    {
        result.reason = "The foreground executable could not be identified.";
        return result;
    }

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = initialized == S_OK || initialized == S_FALSE;
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
    {
        result.reason = "COM could not initialize for UI Automation discovery.";
        return result;
    }
    IUIAutomation* automation = nullptr;
    IUIAutomationElement* root = nullptr;
    IUIAutomationCondition* condition = nullptr;
    IUIAutomationElementArray* elements = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
            reinterpret_cast<void**>(&automation))) || automation == nullptr ||
        FAILED(automation->ElementFromHandle(window, &root)) || root == nullptr ||
        FAILED(automation->CreateTrueCondition(&condition)) || condition == nullptr ||
        FAILED(root->FindAll(TreeScope_Descendants, condition, &elements)) || elements == nullptr)
    {
        result.reason = "Windows UI Automation could not inspect the foreground window.";
        Release(elements);
        Release(condition);
        Release(root);
        Release(automation);
        if (shouldUninitialize) CoUninitialize();
        return result;
    }

    int count = 0;
    elements->get_Length(&count);
    count = std::min(count, std::clamp(maxControls, 1, 5000));
    std::set<std::string> seen;
    for (int index = 0; index < count; ++index)
    {
        IUIAutomationElement* element = nullptr;
        if (FAILED(elements->GetElement(index, &element)) || element == nullptr) continue;
        BOOL enabled = FALSE;
        BOOL offscreen = TRUE;
        element->get_CurrentIsEnabled(&enabled);
        element->get_CurrentIsOffscreen(&offscreen);
        DiscoverableControl control;
        control.name = Property(element, &IUIAutomationElement::get_CurrentName);
        control.automationId = Property(element, &IUIAutomationElement::get_CurrentAutomationId);
        control.permissionKey = !control.automationId.empty() ? control.automationId : control.name;
        element->get_CurrentControlType(&control.controlType);
        control.supportsInvoke = Supports(element, UIA_InvokePatternId);
        control.supportsValue = Supports(element, UIA_ValuePatternId);
        Release(element);
        if (!enabled || offscreen || control.permissionKey.empty() ||
            (!control.supportsInvoke && !control.supportsValue) ||
            !seen.insert(control.permissionKey).second)
        {
            continue;
        }
        result.controls.push_back(std::move(control));
    }
    Release(elements);
    Release(condition);
    Release(root);
    Release(automation);
    if (shouldUninitialize) CoUninitialize();
    result.succeeded = true;
    result.reason = "Discovered " + std::to_string(result.controls.size()) +
        " actionable foreground controls. No permission was changed.";
#else
    (void)maxControls;
    result.reason = "Application control discovery is currently implemented for Windows.";
#endif
    return result;
}

} // namespace revia::actions::windows
