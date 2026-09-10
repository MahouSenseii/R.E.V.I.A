#include "Windows/targetBinding.h"

#include "Actions/actionTypes.h"

#ifdef _WIN32
#include "Windows/uiaElementLocator.h"

#include <windows.h>
#include <uiautomation.h>
#endif

namespace revia::actions::windows
{

bool TargetBinding::Describes(const TargetBinding& other) const
{
    // Window first: the cheap check, and the one that catches a switch to another
    // program or another document of the same program.
    if (window != other.window || processId != other.processId)
    {
        return false;
    }
    // Then the control. A runtime id is the strongest short-lived signal, so when both
    // sides have one it decides.
    if (!runtimeId.empty() && !other.runtimeId.empty())
    {
        return runtimeId == other.runtimeId;
    }
    // Without one, agreement has to come from the rest of the evidence together --
    // including where the control is.
    //
    // Position matters more than it looks. Two unlabelled text fields in one window have
    // no name, no automation id and the same control type, so every other field here
    // compares equal and "all empty" reads as "the same control". That is how focus
    // moving between two fields went unnoticed until a native test put two of them in
    // one window. Where a control sits is evidence, and it is the only evidence left
    // when the rest is blank.
    return automationId == other.automationId && controlType == other.controlType &&
        controlName == other.controlName && isPassword == other.isPassword &&
        left == other.left && top == other.top &&
        right == other.right && bottom == other.bottom;
}

bool IsFresh(
    const TargetBinding& binding,
    const std::chrono::steady_clock::time_point now,
    const std::chrono::milliseconds limit)
{
    if (!binding.valid)
    {
        return false;
    }
    // Monotonic. Wall-clock would let a clock adjustment extend a binding's life.
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - binding.observedAt);
    return age >= std::chrono::milliseconds::zero() && age <= limit;
}

std::string CompareBindings(const TargetBinding& authorized, const TargetBinding& current)
{
    if (!authorized.valid)
    {
        return "the authorized target was never observed";
    }
    if (!current.valid)
    {
        return "the target can no longer be observed";
    }
    if (authorized.window != current.window)
    {
        return "a different window now has the target";
    }
    if (authorized.processId != current.processId)
    {
        return "the window now belongs to a different process";
    }
    if (authorized.contextFingerprint != current.contextFingerprint)
    {
        return "the window or dialog context changed";
    }
    if (!authorized.Describes(current))
    {
        return "a different control is now in that position";
    }
    if (authorized.isPassword != current.isPassword)
    {
        return "the field changed to or from a password field";
    }
    return {};
}

#ifdef _WIN32

namespace
{

std::string Fingerprint(const TargetBinding& binding, const std::string& windowTitle)
{
    // Deliberately includes the control identity: a dialog opening over the target
    // changes what "here" means even when the handle underneath is the same.
    return windowTitle + '|' + binding.runtimeId + '|' + binding.automationId + '|' +
        std::to_string(binding.controlType) + '|' + binding.controlName;
}

TargetBinding FromElement(
    IUIAutomationElement* element,
    const HWND window,
    const std::string& windowTitle,
    const std::string& taskOrigin,
    const std::string& policyVersion,
    const bool focused)
{
    TargetBinding binding;
    binding.id = NewActionId();
    binding.observedAt = std::chrono::steady_clock::now();
    binding.taskOrigin = taskOrigin;
    binding.policyVersion = policyVersion;
    binding.wasFocused = focused;

    if (window != nullptr)
    {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        binding.window = window;
        binding.processId = static_cast<std::uint32_t>(processId);
    }

    if (element != nullptr)
    {
        CONTROLTYPEID controlType = 0;
        BOOL isPassword = FALSE;
        element->get_CurrentControlType(&controlType);
        element->get_CurrentIsPassword(&isPassword);
        binding.runtimeId = ElementRuntimeId(element);
        binding.automationId = WideToUtf8(ElementAutomationId(element));
        binding.controlName = WideToUtf8(ElementName(element));
        binding.controlType = static_cast<int>(controlType);
        binding.isPassword = isPassword != FALSE;
        const ElementBounds bounds = ElementBoundingRectangle(element);
        binding.left = bounds.left;
        binding.top = bounds.top;
        binding.right = bounds.right;
        binding.bottom = bounds.bottom;
    }

    binding.contextFingerprint = Fingerprint(binding, windowTitle);
    // A binding needs a window to be about. Without one there is nothing to compare
    // against later, which is not a weak binding but an absent one.
    binding.valid = binding.window != nullptr;
    return binding;
}

std::string TitleOf(const HWND window)
{
    if (window == nullptr)
    {
        return {};
    }
    wchar_t title[512]{};
    const int length = GetWindowTextW(window, title, 511);
    return length > 0 ? WideToUtf8(std::wstring(title, static_cast<std::size_t>(length)))
                      : std::string{};
}

} // namespace

TargetBinding BindFocusedControl(
    IUIAutomation* automation,
    const std::string& taskOrigin,
    const std::string& policyVersion)
{
    const HWND foreground = GetForegroundWindow();
    IUIAutomationElement* element = nullptr;
    if (automation != nullptr)
    {
        automation->GetFocusedElement(&element);
    }
    TargetBinding binding = FromElement(
        element, foreground, TitleOf(foreground), taskOrigin, policyVersion, true);
    if (element != nullptr) element->Release();
    return binding;
}

TargetBinding BindPoint(
    IUIAutomation* automation,
    const int x,
    const int y,
    const std::string& taskOrigin,
    const std::string& policyVersion)
{
    const POINT point{static_cast<LONG>(x), static_cast<LONG>(y)};
    const HWND window = WindowFromPoint(point);
    IUIAutomationElement* element = nullptr;
    if (automation != nullptr)
    {
        automation->ElementFromPoint(point, &element);
    }
    TargetBinding binding = FromElement(
        element, window, TitleOf(window), taskOrigin, policyVersion, false);
    if (element != nullptr) element->Release();
    return binding;
}

std::string RevalidateFocusBinding(
    IUIAutomation* automation,
    const TargetBinding& authorized,
    const std::chrono::steady_clock::time_point now)
{
    if (!IsFresh(authorized, now))
    {
        return "the observation this was authorized against has expired";
    }
    const TargetBinding current =
        BindFocusedControl(automation, authorized.taskOrigin, authorized.policyVersion);
    return CompareBindings(authorized, current);
}

std::string RevalidatePointBinding(
    IUIAutomation* automation,
    const TargetBinding& authorized,
    const int x,
    const int y,
    const std::chrono::steady_clock::time_point now)
{
    if (!IsFresh(authorized, now))
    {
        return "the observation this was authorized against has expired";
    }
    const TargetBinding current = BindPoint(
        automation, x, y, authorized.taskOrigin, authorized.policyVersion);
    return CompareBindings(authorized, current);
}

#endif // _WIN32

} // namespace revia::actions::windows
