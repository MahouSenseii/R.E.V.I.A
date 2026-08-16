#include "Windows/windowsAutomationExecutor.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <cwctype>

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

    std::wstring Lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t c)
        {
            return static_cast<wchar_t>(std::towlower(c));
        });
        return value;
    }

    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
        {
            return {};
        }
        const int count = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (count <= 0)
        {
            return {};
        }
        std::wstring result(static_cast<std::size_t>(count), L'\0');
        MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), count);
        return result;
    }

    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty())
        {
            return {};
        }
        const int count = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            nullptr, 0, nullptr, nullptr);
        if (count <= 0)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(count), '\0');
        WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), count, nullptr, nullptr);
        return result;
    }

    std::wstring ElementName(IUIAutomationElement* element)
    {
        BSTR value = nullptr;
        if (FAILED(element->get_CurrentName(&value)) || value == nullptr)
        {
            return {};
        }
        std::wstring result(value, SysStringLen(value));
        SysFreeString(value);
        return result;
    }

    std::wstring ProcessFileName(const int processId)
    {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
            static_cast<DWORD>(processId));
        if (process == nullptr)
        {
            return {};
        }
        std::wstring path(32768, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process, 0, path.data(), &length))
        {
            CloseHandle(process);
            return {};
        }
        CloseHandle(process);
        path.resize(length);
        return std::filesystem::path(path).filename().wstring();
    }

    IUIAutomationElement* FindApplicationWindow(
        IUIAutomation* automation,
        const ActionRequest& request)
    {
        IUIAutomationElement* root = nullptr;
        IUIAutomationCondition* condition = nullptr;
        IUIAutomationElementArray* windows = nullptr;
        if (FAILED(automation->GetRootElement(&root)) || root == nullptr ||
            FAILED(automation->CreateTrueCondition(&condition)) || condition == nullptr ||
            FAILED(root->FindAll(TreeScope_Children, condition, &windows)) || windows == nullptr)
        {
            Release(windows);
            Release(condition);
            Release(root);
            return nullptr;
        }

        const std::wstring wantedApplication = Lower(Utf8ToWide(request.application));
        const std::wstring wantedTitle = Lower(Utf8ToWide(request.windowTitle));
        IUIAutomationElement* match = nullptr;
        int length = 0;
        windows->get_Length(&length);
        for (int index = 0; index < length; ++index)
        {
            IUIAutomationElement* candidate = nullptr;
            if (FAILED(windows->GetElement(index, &candidate)) || candidate == nullptr)
            {
                continue;
            }
            int processId = 0;
            candidate->get_CurrentProcessId(&processId);
            const bool applicationMatches =
                Lower(ProcessFileName(processId)) == wantedApplication;
            const std::wstring title = Lower(ElementName(candidate));
            const bool titleMatches = wantedTitle.empty() || title.find(wantedTitle) != std::wstring::npos;
            if (applicationMatches && titleMatches)
            {
                match = candidate;
                break;
            }
            candidate->Release();
        }
        Release(windows);
        Release(condition);
        Release(root);
        return match;
    }

    IUIAutomationElement* FindControl(
        IUIAutomation* automation,
        IUIAutomationElement* window,
        const std::string& control)
    {
        const std::wstring wanted = Utf8ToWide(control);
        VARIANT nameValue;
        VariantInit(&nameValue);
        nameValue.vt = VT_BSTR;
        nameValue.bstrVal = SysAllocStringLen(wanted.data(), static_cast<UINT>(wanted.size()));
        IUIAutomationCondition* nameCondition = nullptr;
        IUIAutomationCondition* idCondition = nullptr;
        IUIAutomationCondition* eitherCondition = nullptr;
        automation->CreatePropertyCondition(UIA_NamePropertyId, nameValue, &nameCondition);
        automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, nameValue, &idCondition);
        VariantClear(&nameValue);
        if (nameCondition == nullptr || idCondition == nullptr ||
            FAILED(automation->CreateOrCondition(nameCondition, idCondition, &eitherCondition)) ||
            eitherCondition == nullptr)
        {
            Release(eitherCondition);
            Release(idCondition);
            Release(nameCondition);
            return nullptr;
        }
        IUIAutomationElement* result = nullptr;
        window->FindFirst(TreeScope_Descendants, eitherCondition, &result);
        Release(eitherCondition);
        Release(idCondition);
        Release(nameCondition);
        return result;
    }

    ActionResult Inspect(IUIAutomation* automation, IUIAutomationElement* window)
    {
        ActionResult result;
        result.attempted = true;
        IUIAutomationCondition* condition = nullptr;
        IUIAutomationElementArray* elements = nullptr;
        if (FAILED(automation->CreateTrueCondition(&condition)) || condition == nullptr ||
            FAILED(window->FindAll(TreeScope_Descendants, condition, &elements)) || elements == nullptr)
        {
            result.message = "Windows UI Automation could not inspect the window.";
            Release(elements);
            Release(condition);
            return result;
        }
        int count = 0;
        elements->get_Length(&count);
        count = std::min(count, 250);
        for (int index = 0; index < count; ++index)
        {
            IUIAutomationElement* element = nullptr;
            if (FAILED(elements->GetElement(index, &element)) || element == nullptr)
            {
                continue;
            }
            const std::wstring name = ElementName(element);
            if (!name.empty())
            {
                CONTROLTYPEID type = 0;
                BOOL isEnabled = FALSE;
                element->get_CurrentControlType(&type);
                element->get_CurrentIsEnabled(&isEnabled);
                std::ostringstream line;
                line << WideToUtf8(name) << " [type=" << type <<
                    ", enabled=" << (isEnabled ? "true" : "false") << ']';
                result.entries.push_back(line.str());
            }
            element->Release();
        }
        result.succeeded = true;
        result.message = "Inspected " + std::to_string(result.entries.size()) +
            " named controls.";
        Release(elements);
        Release(condition);
        return result;
    }
#endif
}

bool WindowsAutomationExecutor::Handles(const ActionType type) const
{
    return type == ActionType::InspectWindow || type == ActionType::FocusWindow ||
        type == ActionType::SetControlText || type == ActionType::InvokeControl;
}

ActionResult WindowsAutomationExecutor::Execute(
    const ActionRequest& request,
    const PolicyDecision&)
{
    ActionResult result;
    result.dryRun = request.dryRun;
    if (request.dryRun)
    {
        result.succeeded = true;
        result.message = "Desktop dry-run passed policy; no UI was changed.";
        return result;
    }
#ifdef _WIN32
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = initialized == S_OK || initialized == S_FALSE;
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
    {
        result.message = "COM could not initialize for Windows UI Automation.";
        return result;
    }

    IUIAutomation* automation = nullptr;
    const HRESULT created = CoCreateInstance(
        CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
        reinterpret_cast<void**>(&automation));
    if (FAILED(created) || automation == nullptr)
    {
        result.message = "Windows UI Automation is unavailable.";
        if (shouldUninitialize) CoUninitialize();
        return result;
    }
    IUIAutomationElement* window = FindApplicationWindow(automation, request);
    if (window == nullptr)
    {
        result.message = "No matching window was found for " + request.application + ".";
        automation->Release();
        if (shouldUninitialize) CoUninitialize();
        return result;
    }

    if (request.type == ActionType::InspectWindow)
    {
        result = Inspect(automation, window);
    }
    else if (request.type == ActionType::FocusWindow)
    {
        result.attempted = true;
        result.succeeded = SUCCEEDED(window->SetFocus());
        result.message = result.succeeded ? "The window received focus." : "The window could not be focused.";
    }
    else
    {
        IUIAutomationElement* control = FindControl(automation, window, request.control);
        result.attempted = true;
        if (control == nullptr)
        {
            result.message = "No matching control was found.";
        }
        else if (request.type == ActionType::SetControlText)
        {
            IUnknown* pattern = nullptr;
            IUIAutomationValuePattern* valuePattern = nullptr;
            if (SUCCEEDED(control->GetCurrentPattern(UIA_ValuePatternId, &pattern)) && pattern != nullptr)
            {
                pattern->QueryInterface(IID_IUIAutomationValuePattern,
                    reinterpret_cast<void**>(&valuePattern));
            }
            const std::wstring value = Utf8ToWide(request.value);
            BSTR valueText = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
            result.succeeded = valuePattern != nullptr && valueText != nullptr &&
                SUCCEEDED(valuePattern->SetValue(valueText));
            SysFreeString(valueText);
            result.message = result.succeeded
                ? "The control text was updated."
                : "The control does not expose a writable Value pattern.";
            Release(valuePattern);
            Release(pattern);
        }
        else
        {
            IUnknown* pattern = nullptr;
            IUIAutomationInvokePattern* invokePattern = nullptr;
            if (SUCCEEDED(control->GetCurrentPattern(UIA_InvokePatternId, &pattern)) && pattern != nullptr)
            {
                pattern->QueryInterface(IID_IUIAutomationInvokePattern,
                    reinterpret_cast<void**>(&invokePattern));
            }
            result.succeeded = invokePattern != nullptr && SUCCEEDED(invokePattern->Invoke());
            result.message = result.succeeded
                ? "The control was invoked."
                : "The control does not expose an Invoke pattern.";
            Release(invokePattern);
            Release(pattern);
        }
        Release(control);
    }
    Release(window);
    Release(automation);
    if (shouldUninitialize) CoUninitialize();
    return result;
#else
    result.message = "Windows UI Automation is only available on Windows.";
    return result;
#endif
}

} // namespace revia::actions::windows
