#include "Windows/windowsAutomationExecutor.h"

#include "Policy/desktopAuthorization.h"
#include "Windows/uiaElementLocator.h"
#include "Windows/targetBinding.h"

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
        int processId = 0;
        window->get_CurrentProcessId(&processId);
        result.content = "Application: " + WideToUtf8(ProcessFileName(processId)) +
            "\nWindow: " + WideToUtf8(ElementName(window));
        UIA_HWND handle = nullptr;
        if (SUCCEEDED(window->get_CurrentNativeWindowHandle(&handle)))
            result.content += std::string("\nForeground: ") +
                (reinterpret_cast<HWND>(handle) == GetForegroundWindow() ? "true" : "false");
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
            CONTROLTYPEID type = 0;
            element->get_CurrentControlType(&type);
            if (!name.empty() || type == UIA_EditControlTypeId)
            {
                BOOL isEnabled = FALSE;
                element->get_CurrentIsEnabled(&isEnabled);
                const std::string automationId = WideToUtf8(ElementAutomationId(element));
                std::ostringstream line;
                line << WideToUtf8(name) << " [type=" << type <<
                    ", enabled=" << (isEnabled ? "true" : "false");
                if (!automationId.empty())
                {
                    line << ", id=" << automationId;
                }
                BOOL focused = FALSE;
                if (SUCCEEDED(element->get_CurrentHasKeyboardFocus(&focused)) && focused)
                    line << ", focused=true";
                // A name alone cannot verify that typing changed a field. Only read a
                // value when UIA positively identifies a non-password control.
                //
                // Document as well as Edit, and the generalization matrix is what found
                // it. Notepad's editing surface is a Document control, not an Edit, so
                // text placed in it could be typed and never read back: the exact-content
                // check returned Unknown on the first real application it met, while
                // working perfectly on the fixture, whose fields are plain EDITs.
                //
                // The password guard is unchanged and still comes first. A Document
                // control does not report IsPassword, but a control that does report it
                // is excluded before the type is even considered, so widening the type
                // cannot widen what is readable about a password field.
                BOOL password = TRUE;
                const bool readableKind = type == UIA_EditControlTypeId ||
                    type == UIA_DocumentControlTypeId;
                if (SUCCEEDED(element->get_CurrentIsPassword(&password)) && !password &&
                    readableKind)
                {
                    IUIAutomationValuePattern* value = nullptr;
                    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId,
                            IID_IUIAutomationValuePattern, reinterpret_cast<void**>(&value))) && value)
                    {
                        BSTR text = nullptr;
                        if (SUCCEEDED(value->get_CurrentValue(&text)) && text)
                        {
                            std::wstring bounded(text, std::min<UINT>(SysStringLen(text), 512));
                            std::replace(bounded.begin(), bounded.end(), L'\n', L' ');
                            std::replace(bounded.begin(), bounded.end(), L'\r', L' ');
                            line << ", value=" << WideToUtf8(bounded);
                        }
                        SysFreeString(text);
                    }
                    Release(value);
                }
                line << ']';
                result.entries.push_back(line.str());
            }
            element->Release();
        }
        result.succeeded = true;
        result.message = "Inspected " + std::to_string(result.entries.size()) +
            " controls.";
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
    const TargetBinding& binding,
    const ActionRequest& request,
    const CapabilitySettings::DesktopControl& settings,
    std::string& outFailure,
    const policy::DesktopApprovalGate* approvals)
{
    policy::TargetEvidence evidence;
    evidence.resolved = binding.valid;
    evidence.controlName = binding.controlName;
    evidence.automationId = binding.automationId;
    // The surrounding window, so a "Confirm" carries the meaning of the dialog it is in.
    evidence.windowTitle = binding.windowTitle;
    evidence.executable = binding.executable;
    evidence.controlType = binding.controlType;
    evidence.isPassword = binding.isPassword;

    const policy::DesktopOperation operation =
        request.type == ActionType::SetControlText
            ? policy::DesktopOperation::SetValue
            : policy::DesktopOperation::Invoke;
    return policy::AuthorizeOrExplain(
        operation, evidence, settings, request, outFailure, approvals);
}

#endif
} // namespace

WindowsAutomationExecutor::WindowsAutomationExecutor(
    CapabilitySettings::DesktopControl inputSettings,
    std::shared_ptr<policy::DesktopApprovalGate> inputApprovals)
    : settings(std::move(inputSettings)), approvals(std::move(inputApprovals))
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
    if (request.requiresRuntimeGuard && !request.beforeCommit && !request.navigationConstraint.enabled)
    {
        result.message = ValidateActionTarget(request, {});
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
        const TargetBinding approved = BindElement(control, window,
            request.requestedBy, policy::PolicyVersion(settings));
        const auto revalidate = [&]() {
            IUIAutomationElement* current = FindControl(automation, window, request);
            const TargetBinding binding = BindElement(current,
                window, request.requestedBy, policy::PolicyVersion(settings));
            std::string drift = CompareBindings(approved, binding);
            if (drift.empty()) drift = ValidateActionTarget(request, binding);
            Release(control);
            control = current;
            if (!drift.empty()) result.message = "The approved control changed: " + drift + ".";
            return drift.empty();
        };
        result.attempted = true;
        if (control == nullptr)
        {
            result.message = request.resolution.IsUiaElementTarget()
                ? "The vision-resolved UI Automation element changed or disappeared; no action was taken."
                : "No matching control was found.";
        }
        else if (!(result.message = ValidateActionTarget(request, approved)).empty())
        {
            // The navigation observation no longer matches, or a resumed action lost
            // the runtime validation its persisted obligation still requires.
        }
        else if (!AuthorizeUiaEffect(
            approved, request, settings, result.message, approvals.get()))
        {
            // Already refused, with the reason in result.message. Reaching a consequence
            // through a control pattern costs the same authority as reaching it with the
            // mouse; which route was chosen is not a property of the consequence.
        }
        else if (!revalidate())
        {
            // The target shown during approval no longer describes this control.
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
            const std::string refusal = request.beforeCommit ? request.beforeCommit() : std::string{};
            if (!refusal.empty()) result.message = "Submission refused: " + refusal + ".";
            else if (!revalidate())
            {
                // Draft inspection can yield to the application too; keep the approved
                // control binding as the last check before its pattern is invoked.
            }
            else
            {
                if (invokePattern != nullptr && request.onCommitStarted)
                    request.onCommitStarted(approved.controlName);
                result.succeeded = invokePattern != nullptr && SUCCEEDED(invokePattern->Invoke());
                result.message = result.succeeded
                    ? "The control was invoked."
                    : "The control does not expose an Invoke pattern.";
            }
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
