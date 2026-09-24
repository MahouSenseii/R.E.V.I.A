#include "Windows/desktopObserver.h"

#include "Windows/uiaElementLocator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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

// What UI Automation says labels this element.
//
// The provider-supplied relationship, not a geometric guess about which static happens
// to sit to the left. Win32's UIA provider fills this in for a control that follows a
// label in tab order, which is how most dialogs are built -- and it returns nothing for
// a field that genuinely has no label, which is the answer that matters.
std::wstring ElementLabelledBy(IUIAutomationElement* element)
{
    VARIANT value;
    VariantInit(&value);
    std::wstring label;
    if (SUCCEEDED(element->GetCurrentPropertyValue(UIA_LabeledByPropertyId, &value)))
    {
        if (value.vt == VT_UNKNOWN && value.punkVal != nullptr)
        {
            IUIAutomationElement* labelElement = nullptr;
            if (SUCCEEDED(value.punkVal->QueryInterface(
                    IID_PPV_ARGS(&labelElement))) && labelElement != nullptr)
            {
                label = ElementName(labelElement);
                labelElement->Release();
            }
        }
    }
    VariantClear(&value);
    return label;
}

// The nearest ancestor that has a name, walking up no further than the window itself.
//
// Bounded to a handful of steps on purpose. A deep tree would otherwise cost a walk per
// element, and the useful container is never far: it is the panel, group or list the
// control sits in, not the application root.
std::wstring NearestNamedAncestor(
    IUIAutomation* automation,
    IUIAutomationElement* element,
    IUIAutomationElement* stopAt)
{
    IUIAutomationTreeWalker* walker = nullptr;
    if (FAILED(automation->get_ControlViewWalker(&walker)) || walker == nullptr)
    {
        return {};
    }

    std::wstring found;
    IUIAutomationElement* current = element;
    current->AddRef();
    for (int depth = 0; depth < 6 && current != nullptr; ++depth)
    {
        IUIAutomationElement* parent = nullptr;
        if (FAILED(walker->GetParentElement(current, &parent)) || parent == nullptr)
        {
            current->Release();
            current = nullptr;
            break;
        }
        current->Release();
        current = parent;

        BOOL same = FALSE;
        if (stopAt != nullptr &&
            SUCCEEDED(automation->CompareElements(current, stopAt, &same)) && same)
        {
            break;
        }
        const std::wstring name = ElementName(current);
        if (!name.empty())
        {
            found = name;
            break;
        }
    }
    if (current != nullptr) current->Release();
    walker->Release();
    return found;
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

namespace
{
// Process-wide and monotonic. Every observation, successful or not, takes the next one.
std::atomic<std::uint64_t> observationGeneration{0};

std::uint64_t SteadyMilliseconds()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

std::string CompareVisualTarget(
    const ActionRequest::ElementResolutionEvidence& target,
    const VisualTargetFacts& current)
{
    if (!target.IsVisualRegionTarget())
    {
        return "That target is not a visually grounded one.";
    }
    if (!target.HasRegion())
    {
        return "That visual target carries no region.";
    }
    if (target.observationGeneration == 0 || target.observedWindow == nullptr)
    {
        return "That visual target is not bound to an observation of the screen.";
    }
    // Something has been looked at since. The region describes a screen that has been
    // replaced, which is what happens on navigation, on a scroll, and when a dialog
    // opens -- none of which move the window, and none of which the checks below would
    // otherwise catch.
    if (current.latestGeneration > target.observationGeneration)
    {
        return "The screen has been observed again since that target was chosen, so it "
            "describes an older view. Look again before acting.";
    }
    if (current.nowMs >= target.observedAtMs &&
        current.nowMs - target.observedAtMs > VisualTargetFreshnessMs)
    {
        return "That visual target is too old to act on. Look again.";
    }
    if (current.foregroundWindow != target.observedWindow ||
        current.foregroundProcessId != target.observedProcessId)
    {
        return "A different window is in front than the one that target was seen in, so "
            "nothing was clicked.";
    }
    if (current.windowBoundsKnown &&
        (current.windowLeft != target.observedWindowLeft ||
         current.windowTop != target.observedWindowTop ||
         current.windowRight != target.observedWindowRight ||
         current.windowBottom != target.observedWindowBottom))
    {
        // Moved or resized. The region was measured in screen space against the old
        // rectangle, so every coordinate derived from it now points somewhere else.
        return "That window has moved or been resized since the target was seen, so the "
            "region no longer points at it.";
    }
    const int x = target.RegionCentreX();
    const int y = target.RegionCentreY();
    if (current.windowBoundsKnown &&
        (x < current.windowLeft || x >= current.windowRight ||
         y < current.windowTop || y >= current.windowBottom))
    {
        return "That target's region is outside the window it was seen in.";
    }
    return {};
}

std::uint64_t DesktopObserver::LatestGeneration()
{
    return observationGeneration.load(std::memory_order_acquire);
}

DesktopObservation DesktopObserver::Observe(const std::size_t maximumControls) const
{
    DesktopObservation observation;
    // Claimed before anything can fail, so a failed look still supersedes an earlier
    // one. The id carries the generation so an audit record and a log line can be
    // matched up without a second field to keep in step.
    observation.generation =
        observationGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    observation.id = "observation-" + std::to_string(observation.generation);
    observation.observedAtMs = SteadyMilliseconds();
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
    observation.foregroundWindow = static_cast<void*>(foreground);
    observation.foregroundProcessId = static_cast<std::uint32_t>(processId);
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

        // Two passes, named controls first.
        //
        // ISSUE-REVIA-0070: the single pass here used to drop every element with an
        // empty accessible name, which is the ordinary shape of an unlabelled input. A
        // nameless edit box was invisible to every decision, so the only way to reach
        // one was a raw coordinate -- strictly more authority for strictly less
        // evidence, and exactly the trade this design exists to avoid.
        //
        // Admitting them is not free: a real window contains thousands of nameless
        // structural elements, and letting those compete for the budget would push out
        // the named controls a decision can actually reason about. So the budget is
        // spent on named controls first and only the remainder is offered to nameless
        // ones, which means this can add candidates and cannot take any away.
        //
        // A nameless element earns its place by being *actionable* -- it advertises a
        // pattern that does something -- and by being *referable*, which means it has an
        // automation id, an inferred label, or a named container. An element that is
        // merely present still gets dropped, because it always was and nothing about it
        // has become useful.
        const auto admit = [&](IUIAutomationElement* element, const bool wantNamed) -> bool
        {
            ObservedControl control;
            control.name = WideToUtf8(ElementName(element));
            const bool named = !control.name.empty();
            if (named != wantNamed) return false;

            const ElementBounds bounds = ElementBoundingRectangle(element);
            const bool offscreen = BoolProperty(element, UIA_IsOffscreenPropertyId);
            if (!bounds.valid || offscreen) return false;

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
            control.invokable =
                BoolProperty(element, UIA_IsInvokePatternAvailablePropertyId);
            control.editable = BoolProperty(element, UIA_IsValuePatternAvailablePropertyId);
            control.toggleable =
                BoolProperty(element, UIA_IsTogglePatternAvailablePropertyId);
            control.selectable =
                BoolProperty(element, UIA_IsSelectionItemPatternAvailablePropertyId);
            // Read for every control, named or not. A password box that published a
            // name was always protected by the consequence classifier reading that
            // name; one that publishes nothing would not have been, and admitting
            // nameless controls is precisely what would have exposed it.
            control.isPassword = BoolProperty(element, UIA_IsPasswordPropertyId);

            if (!named)
            {
                control.nameless = true;
                control.inferredLabel = WideToUtf8(ElementLabelledBy(element));
                control.containerName =
                    WideToUtf8(NearestNamedAncestor(automation, element, window));

                const bool actionable = control.invokable || control.editable ||
                    control.toggleable || control.selectable;
                const bool referable = !control.automationId.empty() ||
                    !control.inferredLabel.empty() || !control.containerName.empty();
                if (!actionable || !referable || !control.enabled) return false;
            }
            else
            {
                // Cheap for a named control and occasionally decisive: two buttons with
                // the same label in different panels are told apart by this and by
                // nothing else.
                control.containerName =
                    WideToUtf8(NearestNamedAncestor(automation, element, window));
            }

            if (observation.controls.size() >= maximumControls)
            {
                ++observation.omittedControls;
                return false;
            }
            observation.controls.push_back(std::move(control));
            return true;
        };

        for (const bool wantNamed : {true, false})
        {
            for (int index = 0; index < count; ++index)
            {
                IUIAutomationElement* element = nullptr;
                if (FAILED(elements->GetElement(index, &element)) || element == nullptr)
                {
                    continue;
                }
                static_cast<void>(admit(element, wantNamed));
                Release(element);
            }
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
