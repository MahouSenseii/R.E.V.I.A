#include "Windows/desktopObserver.h"

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

// A control name is written by whatever application is in front, so it reaches a prompt
// bounded and on one line. A window that puts a paragraph in an accessible name should
// not be able to spend the whole decision budget.
constexpr std::size_t MaximumNameBytes = 120;

std::string BoundedLine(std::string value, const std::size_t limit = MaximumNameBytes)
{
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    if (value.size() > limit)
    {
        // Trimmed on a UTF-8 boundary so a cut multi-byte character cannot corrupt the
        // line it lands in.
        std::size_t end = limit;
        while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U)
        {
            --end;
        }
        value.resize(end);
        value += "...";
    }
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

bool BoolProperty(IUIAutomationElement* element, const PROPERTYID property)
{
    VARIANT value;
    VariantInit(&value);
    const bool read = SUCCEEDED(element->GetCurrentPropertyValue(property, &value)) &&
        value.vt == VT_BOOL && value.boolVal != VARIANT_FALSE;
    VariantClear(&value);
    return read;
}

#endif

} // namespace

std::string DesktopObservation::Fingerprint() const
{
    if (!succeeded)
    {
        return "unobserved:" + failure;
    }
    std::ostringstream stream;
    stream << foregroundApplication << '|' << foregroundTitle << '|'
           << windowLeft << ',' << windowTop << ',' << windowRight << ',' << windowBottom
           << '|' << controls.size() << '+' << omittedControls;
    for (const ObservedControl& control : controls)
    {
        // Position is part of it: a dialog that moved, a list that scrolled, and a
        // button that became enabled are all changes an action might have caused.
        stream << '#' << control.runtimeId << ':' << control.controlType << ':'
               << control.name << ':' << control.left << ',' << control.top
               << ',' << control.right << ',' << control.bottom
               << (control.enabled ? 'e' : '-');
    }
    return stream.str();
}

std::string DesktopObservation::Describe(const std::size_t maximumControls) const
{
    std::ostringstream stream;
    if (!succeeded)
    {
        stream << "The desktop could not be observed: " << failure << '\n';
        return stream.str();
    }

    stream << "Foreground: " << foregroundApplication;
    if (!foregroundTitle.empty())
    {
        stream << "  window \"" << BoundedLine(foregroundTitle) << '"';
    }
    stream << "\nWindow bounds: " << windowLeft << ',' << windowTop << " to "
           << windowRight << ',' << windowBottom << '\n';

    if (controls.empty())
    {
        stream << "No usable controls are visible in it.\n";
        return stream.str();
    }

    stream << "Visible controls (name, type, centre, what it supports):\n";
    const std::size_t shown = std::min(maximumControls, controls.size());
    for (std::size_t index = 0; index < shown; ++index)
    {
        const ObservedControl& control = controls[index];
        stream << "  \"" << BoundedLine(control.name) << "\"  type=" << control.controlType
               << "  at " << control.CentreX() << ',' << control.CentreY();
        if (!control.automationId.empty())
        {
            stream << "  id=" << BoundedLine(control.automationId, 60);
        }
        if (!control.enabled) stream << "  disabled";
        if (control.invokable) stream << "  invokable";
        if (control.editable) stream << "  editable";
        if (control.toggleable) stream << "  toggleable";
        if (control.selectable) stream << "  selectable";
        stream << '\n';
    }
    const std::size_t hidden = controls.size() - shown + omittedControls;
    if (hidden > 0)
    {
        // Said out loud, because "I did not see it" and "it is not there" are different
        // conclusions and only one of them justifies giving up.
        stream << "  (" << hidden << " more not listed)\n";
    }
    return stream.str();
}

DesktopObservation DesktopObserver::Observe(const std::size_t maximumControls) const
{
    DesktopObservation observation;
#ifdef _WIN32
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = initialized == S_OK || initialized == S_FALSE;
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
    {
        observation.failure = "COM could not initialize for observation.";
        return observation;
    }

    IUIAutomation* automation = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
            reinterpret_cast<void**>(&automation))) || automation == nullptr)
    {
        observation.failure = "Windows UI Automation is unavailable.";
        if (shouldUninitialize) CoUninitialize();
        return observation;
    }

    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr)
    {
        observation.failure = "No window currently has focus.";
        Release(automation);
        if (shouldUninitialize) CoUninitialize();
        return observation;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    observation.foregroundApplication =
        WideToUtf8(ProcessFileName(static_cast<int>(processId)));

    IUIAutomationElement* window = nullptr;
    if (FAILED(automation->ElementFromHandle(foreground, &window)) || window == nullptr)
    {
        observation.failure = "The foreground window could not be read.";
        Release(automation);
        if (shouldUninitialize) CoUninitialize();
        return observation;
    }
    observation.foregroundTitle = WideToUtf8(ElementName(window));
    const ElementBounds windowBounds = ElementBoundingRectangle(window);
    observation.windowLeft = windowBounds.left;
    observation.windowTop = windowBounds.top;
    observation.windowRight = windowBounds.right;
    observation.windowBottom = windowBounds.bottom;

    IUIAutomationCondition* condition = nullptr;
    IUIAutomationElementArray* elements = nullptr;
    if (SUCCEEDED(automation->CreateTrueCondition(&condition)) && condition != nullptr &&
        SUCCEEDED(window->FindAll(TreeScope_Descendants, condition, &elements)) &&
        elements != nullptr)
    {
        int count = 0;
        elements->get_Length(&count);
        for (int index = 0; index < count; ++index)
        {
            IUIAutomationElement* element = nullptr;
            if (FAILED(elements->GetElement(index, &element)) || element == nullptr)
            {
                continue;
            }

            ObservedControl control;
            control.name = WideToUtf8(ElementName(element));
            const ElementBounds bounds = ElementBoundingRectangle(element);
            const bool offscreen = BoolProperty(element, UIA_IsOffscreenPropertyId);
            // An unnamed, invisible, or zero-sized element is not something a decision
            // can act on, and there are thousands of them in a real window.
            if (control.name.empty() || !bounds.valid || offscreen)
            {
                Release(element);
                continue;
            }
            if (observation.controls.size() >= maximumControls)
            {
                ++observation.omittedControls;
                Release(element);
                continue;
            }

            CONTROLTYPEID controlType = 0;
            BOOL isEnabled = FALSE;
            element->get_CurrentControlType(&controlType);
            element->get_CurrentIsEnabled(&isEnabled);
            control.automationId = WideToUtf8(ElementAutomationId(element));
            control.runtimeId = ElementRuntimeId(element);
            control.controlType = static_cast<int>(controlType);
            control.left = bounds.left;
            control.top = bounds.top;
            control.right = bounds.right;
            control.bottom = bounds.bottom;
            control.enabled = isEnabled != FALSE;
            control.invokable = BoolProperty(element, UIA_IsInvokePatternAvailablePropertyId);
            control.editable = BoolProperty(element, UIA_IsValuePatternAvailablePropertyId);
            control.toggleable = BoolProperty(element, UIA_IsTogglePatternAvailablePropertyId);
            control.selectable =
                BoolProperty(element, UIA_IsSelectionItemPatternAvailablePropertyId);
            observation.controls.push_back(std::move(control));
            Release(element);
        }
    }

    Release(elements);
    Release(condition);
    Release(window);
    Release(automation);
    if (shouldUninitialize) CoUninitialize();
    observation.succeeded = true;
    return observation;
#else
    (void)maximumControls;
    observation.failure = "Desktop observation is only available on Windows.";
    return observation;
#endif
}

} // namespace revia::actions::windows
