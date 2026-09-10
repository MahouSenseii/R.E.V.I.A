#include "Windows/desktopControlExecutor.h"

#include "Policy/desktopAuthorization.h"
#include "Windows/targetBinding.h"
#include "Windows/uiaElementLocator.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <uiautomation.h>
#endif

namespace revia::actions::windows
{

// Splits UTF-16 into Unicode scalar values, keeping a surrogate pair together.
//
// The previous loop walked code units, so a non-BMP character -- an emoji, most
// historic scripts -- was delivered as two separate SendInput calls with a stop check
// between them. Windows composes a pair only when both halves arrive together, so that
// was both a corruption bug and a place where a cancellation could tear a character in
// half.
std::vector<std::wstring> SplitScalars(const std::wstring& text)
{
    std::vector<std::wstring> scalars;
    scalars.reserve(text.size());
    for (std::size_t index = 0; index < text.size();)
    {
        const wchar_t unit = text[index];
        const bool highSurrogate = unit >= 0xD800 && unit <= 0xDBFF;
        const bool pairFollows = highSurrogate && index + 1 < text.size() &&
            text[index + 1] >= 0xDC00 && text[index + 1] <= 0xDFFF;
        if (pairFollows)
        {
            scalars.emplace_back(text.substr(index, 2));
            index += 2;
            continue;
        }
        scalars.emplace_back(1, unit);
        ++index;
    }
    return scalars;
}


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

struct VirtualDesktop
{
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

VirtualDesktop DesktopBounds()
{
    VirtualDesktop bounds;
    bounds.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    bounds.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    bounds.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    bounds.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return bounds;
}

bool OnVirtualDesktop(const int x, const int y)
{
    const VirtualDesktop desktop = DesktopBounds();
    return desktop.width > 1 && desktop.height > 1 &&
        x >= desktop.left && y >= desktop.top &&
        x < desktop.left + desktop.width && y < desktop.top + desktop.height;
}

// SendInput's absolute space is 0..65535 across the whole virtual desktop, which is the
// same space the screen-capture service already reports monitors in, so a coordinate
// that came from what Revia saw and a coordinate injected here mean the same pixel.
bool ToAbsolute(const int x, const int y, LONG& outX, LONG& outY)
{
    const VirtualDesktop desktop = DesktopBounds();
    if (!OnVirtualDesktop(x, y))
    {
        return false;
    }
    outX = static_cast<LONG>(
        (static_cast<long long>(x - desktop.left) * 65535) / (desktop.width - 1));
    outY = static_cast<LONG>(
        (static_cast<long long>(y - desktop.top) * 65535) / (desktop.height - 1));
    return true;
}

bool Send(std::vector<INPUT> events)
{
    if (events.empty())
    {
        return false;
    }
    const UINT sent = SendInput(
        static_cast<UINT>(events.size()), events.data(), sizeof(INPUT));
    return sent == events.size();
}

INPUT MouseEvent(const DWORD flags, const LONG x = 0, const LONG y = 0, const DWORD data = 0)
{
    INPUT event{};
    event.type = INPUT_MOUSE;
    event.mi.dx = x;
    event.mi.dy = y;
    event.mi.mouseData = data;
    event.mi.dwFlags = flags;
    return event;
}

INPUT KeyEvent(const WORD virtualKey, const bool down)
{
    INPUT event{};
    event.type = INPUT_KEYBOARD;
    event.ki.wVk = virtualKey;
    event.ki.wScan = static_cast<WORD>(MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC));
    event.ki.dwFlags = down ? 0U : KEYEVENTF_KEYUP;
    return event;
}

INPUT UnicodeEvent(const wchar_t unit, const bool down)
{
    INPUT event{};
    event.type = INPUT_KEYBOARD;
    event.ki.wScan = static_cast<WORD>(unit);
    event.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0U : KEYEVENTF_KEYUP);
    return event;
}

// Which window, not merely which program.
//
// A process id alone cannot tell two windows of one program apart, and a browser or an
// editor with two documents open is the ordinary case, not an exotic one. Binding the
// window handle as well is what makes "the window I decided about" and "the window this
// keystroke is about to reach" the same claim.
//
// The pair is checked together: a dead handle reports no process, and a reused process
// id belongs to a different handle, so neither half can drift alone.
struct WindowIdentity
{
    HWND window = nullptr;
    DWORD processId = 0;

    [[nodiscard]] bool Valid() const { return window != nullptr && processId != 0; }
    [[nodiscard]] bool operator==(const WindowIdentity& other) const
    {
        return window == other.window && processId == other.processId;
    }
};

WindowIdentity IdentityOf(const HWND window)
{
    WindowIdentity identity;
    if (window == nullptr)
    {
        return identity;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    identity.window = window;
    identity.processId = processId;
    return identity;
}

WindowIdentity ForegroundIdentity()
{
    return IdentityOf(GetForegroundWindow());
}

DWORD ForegroundProcessId()
{
    return ForegroundIdentity().processId;
}

HWND NativeWindowHandle(IUIAutomationElement* element)
{
    UIA_HWND handle = nullptr;
    if (element == nullptr ||
        FAILED(element->get_CurrentNativeWindowHandle(&handle)))
    {
        return nullptr;
    }
    return static_cast<HWND>(handle);
}

// Revia may not type or click into Revia.
//
// Her own confirmation dialogs are ordinary windows. Under the confined scope the
// approved-application list keeps her out of them by accident; under whole_desktop
// nothing did, which made "ask the user" and "click the button yourself" the same
// gesture. An approval she can grant herself is not an approval.
//
// Unconditional, and not a capability switch: there is no legitimate reason for
// synthesized input to arrive in the process that is synthesizing it.
bool BelongsToRevia(const WindowIdentity& identity)
{
    return identity.processId != 0 && identity.processId == GetCurrentProcessId();
}

std::string ExecutableOfProcess(const DWORD processId)
{
    return processId == 0
        ? std::string{} : WideToUtf8(ProcessFileName(static_cast<int>(processId)));
}

std::string ExecutableAtPoint(const int x, const int y)
{
    const POINT point{static_cast<LONG>(x), static_cast<LONG>(y)};
    const HWND window = WindowFromPoint(point);
    if (window == nullptr)
    {
        return {};
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return ExecutableOfProcess(processId);
}

// Brings the requested window forward and confirms that *that exact window* is the one
// receiving input, not merely some window of the right program.
//
// Fails closed when the element exposes no window handle: without one there is nothing
// to bind the action to, and an unbindable target is not a target.
bool FocusAndConfirm(
    IUIAutomationElement* window,
    const std::string& application,
    WindowIdentity& outIdentity,
    std::string& outFailure)
{
    const WindowIdentity wanted = IdentityOf(NativeWindowHandle(window));
    if (!wanted.Valid())
    {
        outFailure = "The target window of " + application +
            " exposes no window handle, so input could not be bound to it.";
        return false;
    }

    if (ForegroundIdentity() == wanted)
    {
        outIdentity = wanted;
        return true;
    }
    if (window == nullptr || FAILED(window->SetFocus()))
    {
        outFailure = "The target window of " + application + " could not be focused.";
        return false;
    }
    // Windows can defer a foreground change; a bounded poll is the difference between
    // "the window was not ready yet" and "something else owns the keyboard".
    for (int attempt = 0; attempt < 12; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (ForegroundIdentity() == wanted)
        {
            outIdentity = wanted;
            return true;
        }
    }
    outFailure = "The foreground window is not the requested window of " + application +
        "; no input was synthesized.";
    return false;
}

// The consequence gate.
//
// It runs at the last possible moment, because the name on a button is only knowable
// once there is a button. It can only refuse: everything else -- mode, scope, capability
// switches, risk ceiling, rate limit -- has already had its say by the time this is
// asked, and a permissive answer here does not override any of them.
policy::TargetEvidence ToEvidence(
    const PointDescription& target,
    const std::string& windowTitle,
    const bool stale = false)
{
    policy::TargetEvidence evidence;
    evidence.resolved = target.found;
    evidence.controlName = target.elementName;
    evidence.windowTitle = windowTitle;
    evidence.executable = target.executable;
    evidence.controlType = target.controlType;
    evidence.isPassword = target.isPassword;
    evidence.stale = stale;
    return evidence;
}

// Which chords only move around, and which might do something.
//
// The default direction matters here. Enumerating the chords that commit would mean
// every application-specific shortcut nobody listed is treated as harmless -- and
// ctrl+enter sends a message in a great many programs. So the list is of chords known
// to be navigation, and everything else is assumed to be capable of committing until
// something says otherwise.
policy::DesktopOperation ClassifyChord(const std::string& normalizedChord)
{
    static const std::vector<std::string> navigation = {
        "left", "right", "up", "down", "home", "end", "pageup", "pagedown",
        "tab", "shift+tab", "escape", "ctrl+c", "ctrl+left", "ctrl+right",
        "ctrl+home", "ctrl+end", "ctrl+a", "ctrl+f", "f3", "shift+left",
        "shift+right", "shift+up", "shift+down", "shift+home", "shift+end"};
    return std::find(navigation.begin(), navigation.end(), normalizedChord) !=
        navigation.end()
        ? policy::DesktopOperation::KeyNavigate
        : policy::DesktopOperation::KeyActivate;
}

// Every route in this file asks through the shared component rather than deciding for
// itself, so a pointer, a keystroke and a UI Automation pattern cannot drift apart.
using policy::AuthorizeOrExplain;

std::wstring ResolveExecutable(const std::string& application)
{
    const std::wstring name = Utf8ToWide(application);
    if (name.empty())
    {
        return {};
    }
    std::wstring resolved(MAX_PATH, L'\0');
    const DWORD length = SearchPathW(
        nullptr, name.c_str(), nullptr,
        static_cast<DWORD>(resolved.size()), resolved.data(), nullptr);
    if (length == 0 || length >= resolved.size())
    {
        return {};
    }
    resolved.resize(length);
    return resolved;
}

ActionResult LaunchApplication(
    const ActionRequest& request,
    const PolicyDecision& decision)
{
    ActionResult result;
    result.attempted = true;
    result.backend = "windows_create_process";

    const std::wstring executable = ResolveExecutable(request.application);
    if (executable.empty())
    {
        result.message =
            "Windows could not find " + request.application + " on the search path.";
        return result;
    }

    // The command line is assembled here rather than taken from the model: the only
    // variable part is a path policy already confined to an approved root, so there is
    // no free-form argument string for anything to hide in.
    std::wstring commandLine = L"\"" + executable + L"\"";
    const std::filesystem::path& openTarget =
        decision.canonicalSource.empty() ? request.source : decision.canonicalSource;
    if (!openTarget.empty())
    {
        commandLine += L" \"" + openTarget.wstring() + L"\"";
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');
    if (CreateProcessW(
            executable.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
            CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process) == FALSE)
    {
        result.message = "Windows refused to start " + request.application + ".";
        return result;
    }

    std::ostringstream message;
    message << "Started " << request.application << " as process "
            << process.dwProcessId << '.';
    // Give the window a moment to exist so a following action has something to focus.
    WaitForInputIdle(process.hProcess, 5000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    result.succeeded = true;
    result.message = message.str();
    return result;
}

struct PointerTarget
{
    int x = 0;
    int y = 0;
    int endX = 0;
    int endY = 0;
    bool resolved = false;
    bool aimed = false;
    std::string failure;
};

// Confined scope: the point comes from a re-verified element, or from a coordinate that
// must still land inside the approved window.
PointerTarget ResolveInsideWindow(
    IUIAutomation* automation,
    IUIAutomationElement* window,
    const ActionRequest& request)
{
    PointerTarget target;
    const ElementBounds windowBounds = ElementBoundingRectangle(window);
    if (!windowBounds.valid)
    {
        target.failure = "The target window has no usable bounds on screen.";
        return target;
    }

    if (request.resolution.visionResolved)
    {
        // The coordinate captured when the plan was made is never the coordinate
        // clicked: the element is found again and its current centre is used.
        IUIAutomationElement* element = FindResolvedControl(automation, window, request);
        if (element == nullptr)
        {
            target.failure =
                "The vision-resolved element changed or disappeared; nothing was clicked.";
            return target;
        }
        const ElementBounds bounds = ElementBoundingRectangle(element);
        Release(element);
        if (!bounds.valid)
        {
            target.failure = "The resolved element is not currently visible on screen.";
            return target;
        }
        target.x = bounds.left + (bounds.right - bounds.left) / 2;
        target.y = bounds.top + (bounds.bottom - bounds.top) / 2;
        target.aimed = true;
    }
    else if (request.input.hasPoint)
    {
        target.x = request.input.x;
        target.y = request.input.y;
        target.aimed = true;
    }
    target.endX = request.input.endX;
    target.endY = request.input.endY;

    const auto inside = [&windowBounds](const int x, const int y)
    {
        return x >= windowBounds.left && x < windowBounds.right &&
            y >= windowBounds.top && y < windowBounds.bottom;
    };
    // Even an owner-approved chosen coordinate stays inside the window Revia was given
    // permission to operate. Nothing here can reach another application by arithmetic.
    if (target.aimed && !inside(target.x, target.y))
    {
        target.failure = "The point is outside the approved application's window.";
        return target;
    }
    if (request.input.hasEndPoint && !inside(target.endX, target.endY))
    {
        target.failure = "The drag would end outside the approved application's window.";
        return target;
    }
    target.resolved = true;
    return target;
}

bool MoveTo(const int x, const int y)
{
    LONG absoluteX = 0;
    LONG absoluteY = 0;
    if (!ToAbsolute(x, y, absoluteX, absoluteY))
    {
        return false;
    }
    return Send({MouseEvent(
        MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
        absoluteX, absoluteY)});
}

DWORD ButtonDownFlag(const ActionRequest::DesktopInput::PointerButton button)
{
    switch (button)
    {
        case ActionRequest::DesktopInput::PointerButton::Right:
            return MOUSEEVENTF_RIGHTDOWN;
        case ActionRequest::DesktopInput::PointerButton::Middle:
            return MOUSEEVENTF_MIDDLEDOWN;
        case ActionRequest::DesktopInput::PointerButton::Left:
        default:
            return MOUSEEVENTF_LEFTDOWN;
    }
}

DWORD ButtonUpFlag(const ActionRequest::DesktopInput::PointerButton button)
{
    switch (button)
    {
        case ActionRequest::DesktopInput::PointerButton::Right:
            return MOUSEEVENTF_RIGHTUP;
        case ActionRequest::DesktopInput::PointerButton::Middle:
            return MOUSEEVENTF_MIDDLEUP;
        case ActionRequest::DesktopInput::PointerButton::Left:
        default:
            return MOUSEEVENTF_LEFTUP;
    }
}

// A drag is the one action that leaves the machine in a changed state partway through.
// The release is therefore unconditional: every early exit still lets go of the button.
ActionResult Drag(
    const ActionRequest& request,
    const PointerTarget& target,
    policy::DesktopInputGuard& guard,
    IUIAutomation* automation,
    const TargetBinding& destination)
{
    ActionResult result;
    result.attempted = true;
    result.backend = "windows_send_input";
    if (!MoveTo(target.x, target.y))
    {
        result.message = "The drag start point is not on any attached display.";
        return result;
    }
    if (!OnVirtualDesktop(target.endX, target.endY))
    {
        result.message = "The drag end point is not on any attached display.";
        return result;
    }

    const DWORD down = ButtonDownFlag(request.input.button);
    const DWORD up = ButtonUpFlag(request.input.button);
    if (!Send({MouseEvent(down)}))
    {
        result.message = "Windows rejected the synthesized button press.";
        return result;
    }

    // Interpolated rather than teleported: a drag that jumps in one step is not a drag
    // as far as most applications are concerned, because they never see it move.
    constexpr int Steps = 24;
    bool interrupted = false;
    bool delivered = true;
    for (int step = 1; step <= Steps && delivered; ++step)
    {
        if (guard.IsTripped())
        {
            interrupted = true;
            break;
        }
        const int x = target.x + ((target.endX - target.x) * step) / Steps;
        const int y = target.y + ((target.endY - target.y) * step) / Steps;
        delivered = MoveTo(x, y);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    // Where the button comes up is the whole consequence of a drag. Dragging onto a
    // folder and dragging onto a Delete target are the same gesture, and the pointer has
    // been travelling for a fifth of a second: the destination authorized before
    // mouse-down is a claim about the past. Re-checked here, at the last moment it can
    // still be acted on.
    std::string destinationDrift;
    if (!interrupted && delivered && destination.valid)
    {
        destinationDrift = RevalidatePointBinding(
            automation, destination, target.endX, target.endY,
            std::chrono::steady_clock::now());
    }

    if (!destinationDrift.empty())
    {
        // Nowhere safe to drop. Return to where the drag began before letting go, so the
        // item lands back where it started instead of wherever the pointer happens to be.
        static_cast<void>(MoveTo(target.x, target.y));
    }

    // Unconditional, and last: every path above ends here, so a button Revia pressed is
    // never left held by a refusal, a stop, or a rejected move.
    const bool released = Send({MouseEvent(up)});

    result.succeeded =
        delivered && released && !interrupted && destinationDrift.empty();
    result.message = interrupted
        ? "The drag was stopped and the button released: " + guard.Reason()
        : !destinationDrift.empty()
            ? "The drop was refused and the item returned to where it started: " +
                destinationDrift + "."
            : !released
                ? "The drag finished but Windows rejected the button release."
                : !delivered
                    ? "The drag was interrupted by a rejected move; the button was released."
                    : "Dragged from " + std::to_string(target.x) + ", " +
                        std::to_string(target.y) + " to " + std::to_string(target.endX) +
                        ", " + std::to_string(target.endY) + ".";
    return result;
}

ActionResult TypeTextInput(
    const ActionRequest& request,
    const CapabilitySettings::DesktopControl& settings,
    policy::DesktopInputGuard& guard,
    const WindowIdentity& bound,
    IUIAutomation* automation,
    const TargetBinding& control)
{
    ActionResult result;
    result.attempted = true;
    result.backend = "windows_send_input";
    // Policy already applied this ceiling. Re-checking it at the boundary that actually
    // presses the keys means a future caller cannot reach SendInput around it.
    if (request.value.size() > settings.maxTypedCharacters)
    {
        result.message = "The text exceeds the configured typing length limit.";
        return result;
    }
    const std::wstring text = Utf8ToWide(request.value);
    if (text.empty())
    {
        result.message = "The text could not be encoded for keyboard entry.";
        return result;
    }

    // Bound once, then compared per character.
    //
    // The comparison is window handle and process id together, not the process alone.
    // A process check catches a switch to another program and misses the case that
    // actually happens: a second document in the same program. Text meant for one
    // window must not finish in the other, and two windows of one process share a
    // process id but never a handle.
    if (!bound.Valid())
    {
        result.message = "No window could be bound to, so nothing was typed.";
        return result;
    }

    const std::vector<std::wstring> scalars = SplitScalars(text);
    std::size_t sent = 0;
    std::size_t index = 0;
    // Bounded so a long run cannot continue blindly. Sixteen scalars is a short burst --
    // roughly one SendInput batch -- against a revalidation that costs a UI Automation
    // read. Smaller would spend most of the time re-observing; larger would widen the
    // window in which text can land somewhere it was not authorized to go. The exposure
    // is one chunk either way, and this keeps that chunk small without making ordinary
    // typing crawl.
    constexpr std::size_t ChunkScalars = 16;

    const auto stillAimedCorrectly = [&](std::string& outReason)
    {
        if (guard.IsTripped())
        {
            outReason = guard.Reason();
            return false;
        }
        if (!(ForegroundIdentity() == bound))
        {
            outReason = "focus left the window it was aimed at";
            return false;
        }
        // The window is not enough. Two controls in one window can mean entirely
        // different things, and focus moving from a document to a Send button is a
        // change of consequence with no change of handle.
        if (control.valid)
        {
            const std::string drift = RevalidateFocusBinding(
                automation, control, std::chrono::steady_clock::now());
            if (!drift.empty())
            {
                outReason = drift;
                return false;
            }
        }
        return true;
    };

    while (index < scalars.size())
    {
        std::string reason;
        if (!stillAimedCorrectly(reason))
        {
            result.message = "Typing stopped after " + std::to_string(sent) +
                " characters: " + reason + ".";
            return result;
        }

        // A newline or a tab is a control operation, not a character. Both can move
        // focus or submit, so each is delivered alone and the binding is re-checked
        // afterwards rather than assumed to have survived.
        const std::wstring& scalar = scalars[index];
        if (scalar == L"\n" || scalar == L"\t")
        {
            const WORD key = scalar == L"\n" ? VK_RETURN : VK_TAB;
            if (!Send({KeyEvent(key, true), KeyEvent(key, false)}))
            {
                result.message = "Windows rejected synthesized keyboard input after " +
                    std::to_string(sent) + " characters.";
                return result;
            }
            ++sent;
            ++index;
            // Whatever had focus may not have it now. Anything further has to be
            // authorized against wherever the caret actually went.
            std::string moved;
            if (!stillAimedCorrectly(moved))
            {
                result.message = "Stopped after " + std::to_string(sent) +
                    " characters: the control operation moved focus (" + moved + ").";
                return result;
            }
            continue;
        }

        // One batch per chunk, and a surrogate pair never spans two batches: both code
        // units of one scalar go into the same SendInput call, which is the only way
        // Windows reliably composes them into a single character.
        std::vector<INPUT> batch;
        std::size_t inChunk = 0;
        while (index < scalars.size() && inChunk < ChunkScalars)
        {
            const std::wstring& unitPair = scalars[index];
            if (unitPair == L"\n" || unitPair == L"\t")
            {
                break;
            }
            for (const wchar_t unit : unitPair)
            {
                batch.push_back(UnicodeEvent(unit, true));
                batch.push_back(UnicodeEvent(unit, false));
            }
            ++index;
            ++inChunk;
        }
        if (batch.empty())
        {
            continue;
        }
        if (!Send(std::move(batch)))
        {
            result.message = "Windows rejected synthesized keyboard input after " +
                std::to_string(sent) + " characters.";
            return result;
        }
        sent += inChunk;
    }
    result.succeeded = true;
    // The text itself is never echoed into a message that reaches logs, the UI, or
    // memory. The audit record keeps its length for the same reason.
    result.message = "Typed " + std::to_string(sent) + " characters into " +
        ExecutableOfProcess(bound.processId) + ".";
    return result;
}

ActionResult PressKeyChord(
    const ActionRequest& request,
    policy::DesktopInputGuard* guard,
    const WindowIdentity& bound,
    IUIAutomation* automation,
    const TargetBinding& control)
{
    ActionResult result;
    result.attempted = true;
    result.backend = "windows_send_input";
    KeyChord chord;
    std::string error;
    if (!ParseKeyChord(request.input.keys, chord, error))
    {
        result.message = error;
        return result;
    }

    // Re-checked here rather than trusted from the caller. A chord authorized against a
    // focused text field must not be delivered to a Send button that took focus while
    // the decision was being made, and this is the last point where that is knowable.
    if (guard != nullptr && guard->IsTripped())
    {
        result.message = "The chord was not sent: " + guard->Reason();
        return result;
    }
    if (bound.Valid() && !(ForegroundIdentity() == bound))
    {
        result.message = "The chord was not sent: focus left the window it was aimed at.";
        return result;
    }
    if (control.valid)
    {
        const std::string drift = RevalidateFocusBinding(
            automation, control, std::chrono::steady_clock::now());
        if (!drift.empty())
        {
            result.message = "The chord was not sent: " + drift + ".";
            return result;
        }
    }

    std::vector<INPUT> events;
    for (const int modifier : chord.modifierVirtualKeys)
    {
        events.push_back(KeyEvent(static_cast<WORD>(modifier), true));
    }
    events.push_back(KeyEvent(static_cast<WORD>(chord.virtualKey), true));
    events.push_back(KeyEvent(static_cast<WORD>(chord.virtualKey), false));
    for (auto modifier = chord.modifierVirtualKeys.rbegin();
         modifier != chord.modifierVirtualKeys.rend(); ++modifier)
    {
        events.push_back(KeyEvent(static_cast<WORD>(*modifier), false));
    }
    // One SendInput call, so a modifier can never be left held by a partial batch.
    result.succeeded = Send(std::move(events));
    result.message = result.succeeded
        ? "Pressed " + chord.normalized + "."
        : "Windows rejected the synthesized key chord.";
    return result;
}

#endif // _WIN32
} // namespace

DesktopControlExecutor::DesktopControlExecutor(
    CapabilitySettings::DesktopControl inputSettings,
    std::shared_ptr<policy::DesktopInputGuard> inputGuard)
    : settings(std::move(inputSettings)), guard(std::move(inputGuard))
{
}

bool DesktopControlExecutor::Handles(const ActionType type) const
{
    return IsDesktopControlAction(type);
}

ActionResult DesktopControlExecutor::Execute(
    const ActionRequest& request,
    const PolicyDecision& decision)
{
    ActionResult result;
    result.dryRun = request.dryRun;
    if (request.dryRun)
    {
        result.succeeded = true;
        result.message =
            "Desktop-operation dry-run passed policy; no input was synthesized.";
        return result;
    }
    if (!guard)
    {
        // The stop path is not optional equipment. Without it there is no way to
        // interrupt what this executor starts, so it does not start anything.
        result.message = "Desktop control has no emergency stop, so it refused to act.";
        return result;
    }
#ifdef _WIN32
    // Sampled before anything is injected, and only here: Revia's own modifier
    // keystrokes would otherwise look exactly like the physical stop hold.
    if (guard->CheckPhysicalStop())
    {
        result.message = "Desktop control is stopped: " + guard->Reason();
        return result;
    }

    if (request.type == ActionType::LaunchApplication)
    {
        return LaunchApplication(request, decision);
    }

    // Naming an application asks for the confined form; leaving it out asks for the
    // desktop. Policy has already refused the second unless the owner widened the scope.
    const bool screenSpace = request.application.empty();

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = initialized == S_OK || initialized == S_FALSE;
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
    {
        result.message = "COM could not initialize for desktop operation.";
        return result;
    }

    IUIAutomation* automation = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
            reinterpret_cast<void**>(&automation))) || automation == nullptr)
    {
        // Confined actions need it to verify the window. Screen-space actions only use
        // it to describe what they touched, which is worth losing but not worth failing.
        if (!screenSpace)
        {
            result.message = "Windows UI Automation is unavailable, so the target window "
                "cannot be verified and no input was synthesized.";
            if (shouldUninitialize) CoUninitialize();
            return result;
        }
    }

    IUIAutomationElement* window = nullptr;
    WindowIdentity bound;
    result.attempted = true;
    result.backend = "windows_send_input";
    if (!screenSpace)
    {
        window = FindApplicationWindow(automation, request);
        if (window == nullptr)
        {
            result.message = "No matching window was found for " + request.application + ".";
            Release(automation);
            if (shouldUninitialize) CoUninitialize();
            return result;
        }
        std::string focusFailure;
        if (!FocusAndConfirm(window, request.application, bound, focusFailure))
        {
            result.message = focusFailure;
            Release(window);
            Release(automation);
            if (shouldUninitialize) CoUninitialize();
            return result;
        }
    }
    else
    {
        // Screen space names no window, so the binding is whatever is in front at the
        // moment the decision is acted on. Captured here so every check below compares
        // against one fixed answer rather than re-asking a question that can change.
        bound = ForegroundIdentity();
    }

    const auto finish = [&]()
    {
        Release(window);
        Release(automation);
        if (shouldUninitialize) CoUninitialize();
        return result;
    };

    if (request.type == ActionType::PressKeys || request.type == ActionType::TypeText)
    {
        // A shell reached by keystroke is still model text reaching a shell, and in
        // screen space the only way to know which window will receive it is to look.
        if (screenSpace && !settings.allowCommandSurfaces)
        {
            const std::string focused = ExecutableOfProcess(ForegroundProcessId());
            if (IsCommandSurfaceExecutable(focused))
            {
                result.message = "The focused window is " + focused +
                    ", a command surface, so nothing was typed.";
                return finish();
            }
        }
        // Before anything else: she does not get to type into her own windows. An
        // approval dialog she can answer herself authorizes nothing.
        if (BelongsToRevia(ForegroundIdentity()))
        {
            result.message = "Refused: the focused window belongs to Revia herself, and "
                "she does not answer her own dialogs.";
            return finish();
        }

        // Typing goes into whatever holds the caret, so that is what gets classified.
        // The password check is the reliable half of this: a secret field says so
        // itself rather than being inferred from a label.
        //
        // Note there is no `found` guard here. Failing to read the target is not a
        // reason to skip the question -- it is the answer "I do not know what this is",
        // which the authorizer weighs for itself.
        const PointDescription focused = automation != nullptr
            ? DescribeFocusedElement(automation) : PointDescription{};

        policy::DesktopOperation operation;
        if (request.type == ActionType::TypeText)
        {
            // A newline in literal text presses Enter on the way past, which can commit
            // whatever the field belongs to. Text that carries an activation is not the
            // same operation as text that does not.
            operation = request.value.find('\n') != std::string::npos
                ? policy::DesktopOperation::TextEntryWithActivation
                : policy::DesktopOperation::TextEntry;
        }
        else
        {
            KeyChord chord;
            std::string chordError;
            operation = ParseKeyChord(request.input.keys, chord, chordError)
                ? ClassifyChord(chord.normalized)
                : policy::DesktopOperation::KeyActivate;
        }

        std::string refusal;
        if (!AuthorizeOrExplain(operation, ToEvidence(focused, request.windowTitle), settings,
                request, refusal))
        {
            result.message = refusal;
            return finish();
        }

        // Minted after authorization, from what is actually focused right now. This is
        // what every later chunk is compared against, so that focus moving to another
        // control inside the same window ends the run rather than silently redirecting
        // it. The runtime observes and mints; nothing from the request reaches it.
        const TargetBinding typingBinding = automation != nullptr
            ? BindFocusedControl(automation, request.requestedBy, policy::PolicyVersion(settings))
            : TargetBinding{};

        result = request.type == ActionType::PressKeys
            ? PressKeyChord(request, guard.get(), bound, automation, typingBinding)
            : TypeTextInput(request, settings, *guard, bound, automation, typingBinding);
        return finish();
    }

    PointerTarget target;
    if (screenSpace)
    {
        target.x = request.input.x;
        target.y = request.input.y;
        target.endX = request.input.endX;
        target.endY = request.input.endY;
        target.aimed = request.input.hasPoint;
        target.resolved = true;
        if (target.aimed && !OnVirtualDesktop(target.x, target.y))
        {
            result.message = "The point is not on any attached display.";
            return finish();
        }
    }
    else
    {
        target = ResolveInsideWindow(automation, window, request);
        if (!target.resolved)
        {
            result.message = target.failure;
            return finish();
        }
        if (!(ForegroundIdentity() == bound))
        {
            // Re-verified against the exact window, not the program: resolution walks
            // the whole element tree, which is long enough for another document of the
            // same application to come forward.
            result.message = "Focus left the window this was aimed at while the target "
                "was being verified; nothing was clicked.";
            return finish();
        }
    }

    // Where the pointer actually is, for a scroll that named no point.
    if (!target.aimed)
    {
        POINT cursor{};
        if (GetCursorPos(&cursor) != FALSE)
        {
            target.x = static_cast<int>(cursor.x);
            target.y = static_cast<int>(cursor.y);
        }
    }

    if (screenSpace && !settings.allowCommandSurfaces)
    {
        const std::string owner = ExecutableAtPoint(target.x, target.y);
        if (IsCommandSurfaceExecutable(owner))
        {
            result.message = "That point belongs to " + owner +
                ", a command surface, so nothing was clicked.";
            return finish();
        }
    }

    // Same rule for the pointer: she does not click her own dialogs. Checked against the
    // window that owns the pixel she is aiming at, before the pointer ever moves there.
    if (target.aimed && BelongsToRevia(IdentityOf(WindowFromPoint(POINT{
            static_cast<LONG>(target.x), static_cast<LONG>(target.y)}))))
    {
        result.message = "Refused: that point belongs to Revia herself, and she does "
            "not answer her own dialogs.";
        return finish();
    }

    // Which window owns the pixel, captured before the pointer moves. Moving the cursor
    // can itself change what is under it -- a hover menu, a tooltip, a window raised on
    // hover -- so the thing that was decided about has to be re-identified before it is
    // clicked rather than assumed to have stayed put.
    const WindowIdentity aimedAt = target.aimed
        ? IdentityOf(WindowFromPoint(POINT{
            static_cast<LONG>(target.x), static_cast<LONG>(target.y)}))
        : WindowIdentity{};

    if (target.aimed && !MoveTo(target.x, target.y))
    {
        result.message = "The point is not on any attached display.";
        return finish();
    }

    if (target.aimed && request.type != ActionType::MoveCursor)
    {
        const WindowIdentity nowUnderPointer = IdentityOf(WindowFromPoint(POINT{
            static_cast<LONG>(target.x), static_cast<LONG>(target.y)}));
        if (!(nowUnderPointer == aimedAt))
        {
            result.message = "What is under that point changed as the pointer arrived; "
                "nothing was clicked.";
            return finish();
        }
    }

    // What is under the pointer, read after moving. This is the feedback that makes a
    // pointer skill learnable rather than blind; it is description, never permission.
    const PointDescription under = automation != nullptr
        ? DescribePoint(automation, target.x, target.y) : PointDescription{};
    const std::string where = " Pointer is over " + under.Summary() + ".";

    // Moving the pointer commits nothing, so it is not gated. Anything that presses a
    // button is, and the button is only nameable now that the pointer is on it.
    //
    // There is deliberately no `under.found` guard. A failed lookup is not silence, it
    // is the answer "nothing identifiable is there", and the authorizer decides what
    // that means. Skipping the question on a failed read was the old defect: the check
    // passed most reliably exactly when it knew least.
    if (request.type == ActionType::ClickPointer || request.type == ActionType::DragPointer)
    {
        std::string refusal;
        if (!AuthorizeOrExplain(policy::DesktopOperation::PointerActivate,
                ToEvidence(under, request.windowTitle), settings, request, refusal))
        {
            result.message = refusal;
            return finish();
        }
    }

    // A drag ends somewhere else, and where it is released is its own consequence: a
    // file dropped onto a folder and the same file dropped onto a Delete target are the
    // same gesture. The destination is assessed before the button ever goes down, and
    // the binding minted here is what Drag re-checks immediately before releasing.
    TargetBinding dropBinding;
    if (request.type == ActionType::DragPointer && automation != nullptr)
    {
        const PointDescription destination =
            DescribePoint(automation, target.endX, target.endY);
        std::string refusal;
        if (!AuthorizeOrExplain(policy::DesktopOperation::PointerActivate,
                ToEvidence(destination, request.windowTitle), settings, request, refusal))
        {
            result.message = "Refused at the drop point: " + refusal;
            return finish();
        }
        dropBinding = BindPoint(
            automation, target.endX, target.endY, request.requestedBy,
            policy::PolicyVersion(settings));
    }

    if (request.type == ActionType::MoveCursor)
    {
        result.succeeded = true;
        result.message = "Moved the pointer to " + std::to_string(target.x) + ", " +
            std::to_string(target.y) + "." + where;
    }
    else if (request.type == ActionType::DragPointer)
    {
        result = Drag(request, target, *guard, automation, dropBinding);
    }
    else if (request.type == ActionType::ScrollPointer)
    {
        const DWORD flags = request.input.horizontalScroll
            ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
        const DWORD amount = static_cast<DWORD>(
            static_cast<int>(WHEEL_DELTA) * request.input.scrollClicks);
        result.succeeded = Send({MouseEvent(flags, 0, 0, amount)});
        result.message = result.succeeded
            ? "Scrolled " + std::to_string(request.input.scrollClicks) + " detents." + where
            : "Windows rejected the synthesized scroll.";
    }
    else
    {
        const DWORD down = ButtonDownFlag(request.input.button);
        const DWORD up = ButtonUpFlag(request.input.button);
        bool delivered = true;
        bool targetChanged = false;
        int completed = 0;
        for (int click = 0; click < request.input.clickCount && delivered; ++click)
        {
            if (guard->IsTripped())
            {
                break;
            }
            // Re-checked between clicks, not only before the first. The first click is
            // what opens the dialog that the second one would land on, so a target
            // verified once and then trusted for three clicks is verified for the wrong
            // thing.
            if (click > 0)
            {
                const WindowIdentity stillThere = IdentityOf(WindowFromPoint(POINT{
                    static_cast<LONG>(target.x), static_cast<LONG>(target.y)}));
                if (!(stillThere == aimedAt))
                {
                    targetChanged = true;
                    break;
                }
            }
            delivered = Send({MouseEvent(down), MouseEvent(up)});
            if (delivered) ++completed;
        }
        result.succeeded = delivered && !targetChanged &&
            completed == request.input.clickCount;
        result.message = result.succeeded
            ? "Clicked " + std::to_string(completed) + " time(s) at " +
                std::to_string(target.x) + ", " + std::to_string(target.y) + "." + where
            : targetChanged
                ? "Stopped after " + std::to_string(completed) +
                    " click(s): what was under the pointer changed."
                : guard->IsTripped()
                    ? "The click was stopped: " + guard->Reason()
                    : "Windows rejected the synthesized click.";
    }
    return finish();
#else
    (void)decision;
    (void)settings;
    result.message = "Desktop operation is only available on Windows.";
    return result;
#endif
}

} // namespace revia::actions::windows
