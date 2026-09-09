#include "Windows/uiaElementLocator.h"

#ifdef _WIN32

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <sstream>

#include <windows.h>
#include <uiautomation.h>

namespace revia::actions::windows
{

namespace
{

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

} // namespace

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
    if (element == nullptr || FAILED(element->get_CurrentName(&value)) || value == nullptr)
    {
        return {};
    }
    std::wstring result(value, SysStringLen(value));
    SysFreeString(value);
    return result;
}

std::wstring ElementAutomationId(IUIAutomationElement* element)
{
    BSTR value = nullptr;
    if (element == nullptr ||
        FAILED(element->get_CurrentAutomationId(&value)) || value == nullptr)
    {
        return {};
    }
    std::wstring result(value, SysStringLen(value));
    SysFreeString(value);
    return result;
}

std::string ElementRuntimeId(IUIAutomationElement* element)
{
    SAFEARRAY* values = nullptr;
    if (element == nullptr || FAILED(element->GetRuntimeId(&values)) || values == nullptr)
    {
        return {};
    }
    LONG lower = 0;
    LONG upper = -1;
    if (FAILED(SafeArrayGetLBound(values, 1, &lower)) ||
        FAILED(SafeArrayGetUBound(values, 1, &upper)))
    {
        SafeArrayDestroy(values);
        return {};
    }
    std::ostringstream stream;
    for (LONG index = lower; index <= upper; ++index)
    {
        int value = 0;
        if (FAILED(SafeArrayGetElement(values, &index, &value)))
        {
            SafeArrayDestroy(values);
            return {};
        }
        if (index > lower)
        {
            stream << '.';
        }
        stream << value;
    }
    SafeArrayDestroy(values);
    return stream.str();
}

ElementBounds ElementBoundingRectangle(IUIAutomationElement* element)
{
    ElementBounds bounds;
    RECT rectangle{};
    if (element == nullptr || FAILED(element->get_CurrentBoundingRectangle(&rectangle)))
    {
        return bounds;
    }
    bounds.left = static_cast<int>(rectangle.left);
    bounds.top = static_cast<int>(rectangle.top);
    bounds.right = static_cast<int>(rectangle.right);
    bounds.bottom = static_cast<int>(rectangle.bottom);
    bounds.valid = bounds.right > bounds.left && bounds.bottom > bounds.top;
    return bounds;
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
    if (automation == nullptr ||
        FAILED(automation->GetRootElement(&root)) || root == nullptr ||
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
        const bool titleMatches =
            wantedTitle.empty() || title.find(wantedTitle) != std::wstring::npos;
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

IUIAutomationElement* FindResolvedControl(
    IUIAutomation* automation,
    IUIAutomationElement* window,
    const ActionRequest& request)
{
    IUIAutomationCondition* condition = nullptr;
    IUIAutomationElementArray* elements = nullptr;
    if (automation == nullptr || window == nullptr ||
        FAILED(automation->CreateTrueCondition(&condition)) || condition == nullptr ||
        FAILED(window->FindAll(TreeScope_Descendants, condition, &elements)) ||
        elements == nullptr)
    {
        Release(elements);
        Release(condition);
        return nullptr;
    }

    IUIAutomationElement* match = nullptr;
    int count = 0;
    elements->get_Length(&count);
    for (int index = 0; index < count; ++index)
    {
        IUIAutomationElement* candidate = nullptr;
        if (FAILED(elements->GetElement(index, &candidate)) || candidate == nullptr)
        {
            continue;
        }
        CONTROLTYPEID controlType = 0;
        candidate->get_CurrentControlType(&controlType);
        const bool matches =
            ElementRuntimeId(candidate) == request.resolution.resolvedRuntimeId &&
            (request.resolution.resolvedName.empty() ||
                WideToUtf8(ElementName(candidate)) == request.resolution.resolvedName) &&
            (request.resolution.resolvedAutomationId.empty() ||
                WideToUtf8(ElementAutomationId(candidate)) ==
                    request.resolution.resolvedAutomationId) &&
            (request.resolution.resolvedControlType == 0 ||
                controlType == request.resolution.resolvedControlType);
        if (matches)
        {
            match = candidate;
            break;
        }
        Release(candidate);
    }
    Release(elements);
    Release(condition);
    return match;
}

IUIAutomationElement* FindControl(
    IUIAutomation* automation,
    IUIAutomationElement* window,
    const ActionRequest& request)
{
    if (request.resolution.visionResolved)
    {
        // Never fall back to a name or coordinate after a typed resolution. If the
        // exact element disappeared while confirmation was open, refusing is safer.
        return FindResolvedControl(automation, window, request);
    }
    if (automation == nullptr || window == nullptr)
    {
        return nullptr;
    }
    const std::wstring wanted = Utf8ToWide(request.control);
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

} // namespace revia::actions::windows

#endif
