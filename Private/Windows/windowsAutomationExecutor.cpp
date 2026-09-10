#include "Windows/windowsAutomationExecutor.h"

#include "Policy/desktopAuthorization.h"
#include "Windows/uiaElementLocator.h"

#include <algorithm>
#include <sstream>

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
                const std::string automationId = WideToUtf8(ElementAutomationId(element));
                std::ostringstream line;
                line << WideToUtf8(name) << " [type=" << type <<
                    ", enabled=" << (isEnabled ? "true" : "false");
                if (!automationId.empty())
                {
                    line << ", id=" << automationId;
                }
                line << ']';
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

namespace
{
#ifdef _WIN32

// Builds the same evidence the pointer path builds, from the element that is about to be
// driven, and asks the same shared authorizer.
bool AuthorizeUiaEffect(
    IUIAutomationElement* control,
    IUIAutomationElement* window,
    const ActionRequest& request,
    const CapabilitySettings::DesktopControl& settings,
    std::string& outFailure)
{
    policy::TargetEvidence evidence;
    evidence.resolved = control != nullptr;
    evidence.controlName = WideToUtf8(ElementName(control));
    evidence.automationId = WideToUtf8(ElementAutomationId(control));
    // The surrounding window, so a "Confirm" carries the meaning of the dialog it is in.
    evidence.windowTitle = WideToUtf8(ElementName(window));
    evidence.executable = request.application;
    if (control != nullptr)
    {
        CONTROLTYPEID controlType = 0;
        BOOL isPassword = FALSE;
        control->get_CurrentControlType(&controlType);
        control->get_CurrentIsPassword(&isPassword);
        evidence.controlType = static_cast<int>(controlType);
        evidence.isPassword = isPassword != FALSE;
    }

    const policy::DesktopOperation operation =
        request.type == ActionType::SetControlText
            ? policy::DesktopOperation::SetValue
            : policy::DesktopOperation::Invoke;
    return policy::AuthorizeOrExplain(operation, evidence, settings, request, outFailure);
}

#endif
} // namespace

WindowsAutomationExecutor::WindowsAutomationExecutor(
    CapabilitySettings::DesktopControl inputSettings)
    : settings(std::move(inputSettings))
{
}

bool WindowsAutomationExecutor::Handles(const ActionType type) const
{
    return IsUiAutomationAction(type);
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
        IUIAutomationElement* control = FindControl(automation, window, request);
        result.attempted = true;
        if (control == nullptr)
        {
            result.message = request.resolution.visionResolved
                ? "The vision-resolved UI Automation element changed or disappeared; no action was taken."
                : "No matching control was found.";
        }
        else if (!AuthorizeUiaEffect(control, window, request, settings, result.message))
        {
            // Already refused, with the reason in result.message. Reaching a consequence
            // through a control pattern costs the same authority as reaching it with the
            // mouse; which route was chosen is not a property of the consequence.
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
