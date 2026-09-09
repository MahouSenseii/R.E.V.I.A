#include "Windows/desktopControlExecutor.h"

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

std::wstring LowerWide(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t c)
    {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
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

// SendInput's absolute space is 0..65535 across the whole virtual desktop, which is the
// same space the screen-capture service already reports monitors in, so a coordinate
// that came from what Revia saw and a coordinate injected here mean the same pixel.
bool ToAbsolute(const int x, const int y, LONG& outX, LONG& outY)
{
    const VirtualDesktop desktop = DesktopBounds();
    if (desktop.width <= 1 || desktop.height <= 1)
    {
        return false;
    }
    if (x < desktop.left || y < desktop.top ||
        x >= desktop.left + desktop.width || y >= desktop.top + desktop.height)
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

DWORD ForegroundProcessId()
{
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr)
    {
        return 0;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    return processId;
}

// The containment check that matters. Policy proved the application is approved; this
// proves the keystroke or click is about to reach that application and not whatever
// took focus in the meantime.
bool ForegroundBelongsTo(const std::string& application)
{
    const DWORD processId = ForegroundProcessId();
    if (processId == 0)
    {
        return false;
    }
    return LowerWide(ProcessFileName(static_cast<int>(processId))) ==
        LowerWide(Utf8ToWide(application));
}

bool FocusAndConfirm(IUIAutomationElement* window, const std::string& application)
{
    if (ForegroundBelongsTo(application))
    {
        return true;
    }
    if (window == nullptr || FAILED(window->SetFocus()))
    {
        return false;
    }
    // Windows can defer a foreground change; a bounded poll is the difference between
    // "the window was not ready yet" and "something else owns the keyboard".
    for (int attempt = 0; attempt < 12; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (ForegroundBelongsTo(application))
        {
            return true;
        }
    }
    return false;
}

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
    bool resolved = false;
    std::string failure;
};

PointerTarget ResolvePointerTarget(
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
    }
    else
    {
        target.x = request.input.x;
        target.y = request.input.y;
    }

    // Even an owner-approved raw coordinate stays inside the window Revia was given
    // permission to operate. Nothing here can reach another application by arithmetic.
    if (target.x < windowBounds.left || target.x >= windowBounds.right ||
        target.y < windowBounds.top || target.y >= windowBounds.bottom)
    {
        target.failure = "The point is outside the approved application's window.";
        return target;
    }
    target.resolved = true;
    return target;
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

ActionResult TypeTextInput(
    const ActionRequest& request,
    const CapabilitySettings::DesktopControl& settings,
    policy::DesktopInputGuard& guard)
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

    // Pinned once, then compared per character. Comparing process ids rather than
    // re-reading the image name is both cheaper and stricter: a second instance of the
    // same approved executable is a different window, and text meant for one of them
    // should not finish in the other.
    const DWORD target = ForegroundProcessId();
    if (target == 0)
    {
        result.message = "No window has focus, so nothing was typed.";
        return result;
    }

    std::size_t sent = 0;
    for (const wchar_t unit : text)
    {
        if (guard.IsTripped())
        {
            result.message = "Typing stopped after " + std::to_string(sent) +
                " characters: " + guard.Reason();
            return result;
        }
        if (ForegroundProcessId() != target)
        {
            result.message = "Typing stopped after " + std::to_string(sent) +
                " characters because focus left " + request.application + ".";
            return result;
        }
        const bool delivered = unit == L'\n'
            ? Send({KeyEvent(VK_RETURN, true), KeyEvent(VK_RETURN, false)})
            : unit == L'\t'
                ? Send({KeyEvent(VK_TAB, true), KeyEvent(VK_TAB, false)})
                : Send({UnicodeEvent(unit, true), UnicodeEvent(unit, false)});
        if (!delivered)
        {
            result.message = "Windows rejected synthesized keyboard input after " +
                std::to_string(sent) + " characters.";
            return result;
        }
        ++sent;
    }
    result.succeeded = true;
    // The text itself is never echoed into a message that reaches logs, the UI, or
    // memory. The audit record keeps its length for the same reason.
    result.message = "Typed " + std::to_string(sent) + " characters into " +
        request.application + ".";
    return result;
}

ActionResult PressKeyChord(const ActionRequest& request)
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
        ? "Pressed " + chord.normalized + " in " + request.application + "."
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
        result.message = "Windows UI Automation is unavailable, so the target window "
            "cannot be verified and no input was synthesized.";
        if (shouldUninitialize) CoUninitialize();
        return result;
    }

    IUIAutomationElement* window = FindApplicationWindow(automation, request);
    if (window == nullptr)
    {
        result.message = "No matching window was found for " + request.application + ".";
        Release(automation);
        if (shouldUninitialize) CoUninitialize();
        return result;
    }

    result.attempted = true;
    result.backend = "windows_send_input";
    if (!FocusAndConfirm(window, request.application))
    {
        result.message = "The foreground window does not belong to " +
            request.application + "; no input was synthesized.";
    }
    else if (request.type == ActionType::PressKeys)
    {
        result = PressKeyChord(request);
    }
    else if (request.type == ActionType::TypeText)
    {
        result = TypeTextInput(request, settings, *guard);
    }
    else
    {
        const PointerTarget target = ResolvePointerTarget(automation, window, request);
        if (!target.resolved)
        {
            result.message = target.failure;
        }
        else if (!ForegroundBelongsTo(request.application))
        {
            // Re-verified after the target was resolved: resolution walks the whole
            // element tree, which is long enough for focus to move.
            result.message = "Focus left " + request.application +
                " while the target was being verified; nothing was clicked.";
        }
        else if (!MoveTo(target.x, target.y))
        {
            result.message = "The point is not on any attached display.";
        }
        else if (request.type == ActionType::MoveCursor)
        {
            result.succeeded = true;
            result.message = "Moved the pointer to " + std::to_string(target.x) + ", " +
                std::to_string(target.y) + " in " + request.application + ".";
        }
        else if (request.type == ActionType::ScrollPointer)
        {
            const DWORD flags = request.input.horizontalScroll
                ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
            const DWORD amount = static_cast<DWORD>(
                static_cast<int>(WHEEL_DELTA) * request.input.scrollClicks);
            result.succeeded = Send({MouseEvent(flags, 0, 0, amount)});
            result.message = result.succeeded
                ? "Scrolled " + std::to_string(request.input.scrollClicks) +
                    " detents in " + request.application + "."
                : "Windows rejected the synthesized scroll.";
        }
        else
        {
            const DWORD down = ButtonDownFlag(request.input.button);
            const DWORD up = ButtonUpFlag(request.input.button);
            bool delivered = true;
            int completed = 0;
            for (int click = 0; click < request.input.clickCount && delivered; ++click)
            {
                if (guard->IsTripped())
                {
                    break;
                }
                delivered = Send({MouseEvent(down), MouseEvent(up)});
                if (delivered) ++completed;
            }
            result.succeeded = delivered && completed == request.input.clickCount;
            result.message = result.succeeded
                ? "Clicked " + std::to_string(completed) + " time(s) at " +
                    std::to_string(target.x) + ", " + std::to_string(target.y) + " in " +
                    request.application + "."
                : guard->IsTripped()
                    ? "The click was stopped: " + guard->Reason()
                    : "Windows rejected the synthesized click.";
        }
    }

    Release(window);
    Release(automation);
    if (shouldUninitialize) CoUninitialize();
    return result;
#else
    (void)decision;
    (void)settings;
    result.message = "Desktop operation is only available on Windows.";
    return result;
#endif
}

} // namespace revia::actions::windows
