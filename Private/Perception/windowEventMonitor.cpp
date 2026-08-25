#include "Perception/windowEventMonitor.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace revia::perception
{

namespace
{

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

#ifdef _WIN32
WindowEventMonitor* activeMonitor = nullptr;
std::mutex activeMonitorMutex;

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0)
    {
        return {};
    }
    std::string output(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        output.data(), count, nullptr, nullptr);
    return output;
}

std::string WindowTitleOf(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    if (length <= 0)
    {
        return {};
    }
    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, buffer.data(), static_cast<int>(buffer.size()));
    if (copied <= 0)
    {
        return {};
    }
    buffer.resize(static_cast<std::size_t>(copied));
    return WideToUtf8(buffer);
}

std::string ExecutableOf(HWND window)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0)
    {
        return {};
    }
    const HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr)
    {
        return {};
    }
    std::array<wchar_t, MAX_PATH> path{};
    DWORD size = static_cast<DWORD>(path.size());
    const BOOL queried = QueryFullProcessImageNameW(process, 0, path.data(), &size);
    CloseHandle(process);
    if (!queried || size == 0)
    {
        return {};
    }
    const std::wstring full(path.data(), size);
    const std::size_t separator = full.find_last_of(L"\\/");
    return WideToUtf8(separator == std::wstring::npos ? full : full.substr(separator + 1));
}

// A top-level, visible, non-tool window. Without this, every tooltip, menu, and hidden
// helper window generates an event and the signal disappears into the noise.
bool IsInterestingWindow(HWND window)
{
    if (window == nullptr || !IsWindowVisible(window))
    {
        return false;
    }
    if (GetAncestor(window, GA_ROOT) != window)
    {
        return false;
    }
    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    return (exStyle & WS_EX_TOOLWINDOW) == 0;
}

struct MonitorLookup
{
    HMONITOR wanted = nullptr;
    int currentIndex = 0;
    int foundIndex = 0;
};

BOOL CALLBACK FindMonitorIndex(
    const HMONITOR monitor,
    HDC,
    LPRECT,
    const LPARAM data)
{
    auto* lookup = reinterpret_cast<MonitorLookup*>(data);
    ++lookup->currentIndex;
    if (monitor == lookup->wanted)
    {
        lookup->foundIndex = lookup->currentIndex;
        return FALSE;
    }
    return TRUE;
}

void ReadWindowPlacement(const HWND window, WindowObservation& observation)
{
    RECT windowBounds{};
    if (GetWindowRect(window, &windowBounds))
    {
        observation.windowLeft = static_cast<int>(windowBounds.left);
        observation.windowTop = static_cast<int>(windowBounds.top);
        observation.windowRight = static_cast<int>(windowBounds.right);
        observation.windowBottom = static_cast<int>(windowBounds.bottom);
    }

    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr)
    {
        return;
    }
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (GetMonitorInfoW(monitor, &information))
    {
        observation.monitorIsPrimary = (information.dwFlags & MONITORINFOF_PRIMARY) != 0;
        observation.monitorLeft = static_cast<int>(information.rcMonitor.left);
        observation.monitorTop = static_cast<int>(information.rcMonitor.top);
        observation.monitorRight = static_cast<int>(information.rcMonitor.right);
        observation.monitorBottom = static_cast<int>(information.rcMonitor.bottom);
    }
    MonitorLookup lookup;
    lookup.wanted = monitor;
    EnumDisplayMonitors(nullptr, nullptr, FindMonitorIndex,
        reinterpret_cast<LPARAM>(&lookup));
    observation.monitorIndex = lookup.foundIndex;
}

void CALLBACK EventProc(
    HWINEVENTHOOK,
    const DWORD event,
    const HWND window,
    const LONG objectId,
    const LONG childId,
    DWORD,
    DWORD)
{
    // OBJID_WINDOW/CHILDID_SELF only: object-level events fire for controls inside a
    // window, which is a different and far more invasive kind of observation.
    if (objectId != OBJID_WINDOW || childId != CHILDID_SELF)
    {
        return;
    }
    WindowEventMonitor* monitor = nullptr;
    {
        std::lock_guard lock(activeMonitorMutex);
        monitor = activeMonitor;
    }
    if (monitor != nullptr)
    {
        monitor->HandleRawEvent(static_cast<std::uint32_t>(event), window);
    }
}
#endif

} // namespace

std::string ToString(const ObservationKind value)
{
    switch (value)
    {
        case ObservationKind::ForegroundChanged: return "foreground";
        case ObservationKind::WindowOpened: return "opened";
        case ObservationKind::TitleChanged: return "title";
    }
    return "foreground";
}

PerceptionFilter::PerceptionFilter(perceptionSettings settings)
    : configuration(std::move(settings))
{
}

bool PerceptionFilter::IsExcludedApplication(
    const perceptionSettings& settings,
    const std::string& application)
{
    if (application.empty())
    {
        // An application that could not be identified is excluded rather than recorded.
        // Denying by default is the whole posture of this stage.
        return true;
    }
    const std::string lowered = ToLower(application);
    return std::any_of(
        settings.excludedApplications.begin(),
        settings.excludedApplications.end(),
        [&lowered](const std::string& excluded)
        {
            return ToLower(excluded) == lowered;
        });
}

bool PerceptionFilter::IsExcludedTitle(
    const perceptionSettings& settings,
    const std::string& windowTitle)
{
    const std::string lowered = ToLower(windowTitle);
    return std::any_of(
        settings.excludedTitleFragments.begin(),
        settings.excludedTitleFragments.end(),
        [&lowered](const std::string& fragment)
        {
            return !fragment.empty() && lowered.find(ToLower(fragment)) != std::string::npos;
        });
}

Suppression PerceptionFilter::Admit(
    const WindowObservation& candidate,
    const std::chrono::steady_clock::time_point now)
{
    if (IsExcludedApplication(configuration, candidate.application))
    {
        return Suppression::ExcludedApplication;
    }
    if (IsExcludedTitle(configuration, candidate.windowTitle))
    {
        return Suppression::ExcludedTitle;
    }
    if (hasAdmitted &&
        candidate.application == lastApplication &&
        candidate.windowTitle == lastTitle)
    {
        return Suppression::Unchanged;
    }
    if (hasAdmitted &&
        now - lastAdmitted <
            std::chrono::milliseconds(configuration.minimumEventIntervalMs))
    {
        return Suppression::Unchanged;
    }

    if (!hasAdmitted || now - windowStarted >= std::chrono::minutes(1))
    {
        windowStarted = now;
        windowCount = 0;
    }
    if (windowCount >= static_cast<std::uint32_t>(configuration.maxObservationsPerMinute))
    {
        return Suppression::RateLimited;
    }

    ++windowCount;
    lastApplication = candidate.application;
    lastTitle = candidate.windowTitle;
    lastAdmitted = now;
    hasAdmitted = true;
    return Suppression::None;
}

WindowEventMonitor::~WindowEventMonitor()
{
    Shutdown();
}

bool WindowEventMonitor::Start(
    const perceptionSettings& settings,
    ObservationHandler newObservationHandler,
    StatusHandler newStatusHandler)
{
    Shutdown();
    {
        std::lock_guard lock(mutex);
        configuration = settings;
        observationHandler = std::move(newObservationHandler);
        statusHandler = std::move(newStatusHandler);
        filter = PerceptionFilter(settings);
        counters = PerceptionCounters{};
    }

    if (!settings.bEnabled)
    {
        Notify("Off", "Ambient perception is off. Nothing is being observed.");
        return true;
    }

#ifdef _WIN32
    paused.store(false);
    worker = std::jthread([this]() { Run(); });
    return true;
#else
    Notify("Unavailable", "Window observation is implemented for Windows only.");
    return false;
#endif
}

void WindowEventMonitor::Run()
{
#ifdef _WIN32
    workerThreadId.store(GetCurrentThreadId());
    {
        std::lock_guard lock(activeMonitorMutex);
        activeMonitor = this;
    }

    // WINEVENT_OUTOFCONTEXT keeps Revia's code out of every other process. The callback is
    // delivered to this thread's message queue instead, which is why this thread pumps.
    const HWINEVENTHOOK foregroundHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, EventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    const HWINEVENTHOOK objectHook = SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_NAMECHANGE,
        nullptr, EventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (foregroundHook == nullptr && objectHook == nullptr)
    {
        {
            std::lock_guard lock(activeMonitorMutex);
            activeMonitor = nullptr;
        }
        Notify("Error", "Window event hooks could not be installed.");
        return;
    }

    observing.store(true);
    Notify("Watching", "Observing window and focus changes only. No screen capture.");

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    observing.store(false);
    if (foregroundHook != nullptr)
    {
        UnhookWinEvent(foregroundHook);
    }
    if (objectHook != nullptr)
    {
        UnhookWinEvent(objectHook);
    }
    {
        std::lock_guard lock(activeMonitorMutex);
        if (activeMonitor == this)
        {
            activeMonitor = nullptr;
        }
    }
    Notify("Off", "Ambient perception stopped.");
#endif
}

void WindowEventMonitor::HandleRawEvent(
    const std::uint32_t event,
    void* windowHandle)
{
#ifdef _WIN32
    HWND window = static_cast<HWND>(windowHandle);
    if (!IsInterestingWindow(window))
    {
        return;
    }

    WindowObservation candidate;
    switch (event)
    {
        case EVENT_SYSTEM_FOREGROUND:
            candidate.kind = ObservationKind::ForegroundChanged;
            break;
        case EVENT_OBJECT_CREATE:
            candidate.kind = ObservationKind::WindowOpened;
            break;
        case EVENT_OBJECT_NAMECHANGE:
            candidate.kind = ObservationKind::TitleChanged;
            break;
        default:
            return;
    }

    if (paused.load())
    {
        return;
    }

    candidate.application = ExecutableOf(window);
    candidate.windowTitle = WindowTitleOf(window);
    ReadWindowPlacement(window, candidate);

    ObservationHandler handler;
    Suppression verdict = Suppression::None;
    {
        std::lock_guard lock(mutex);
        verdict = filter.Admit(candidate, std::chrono::steady_clock::now());
        switch (verdict)
        {
            case Suppression::None: ++counters.observed; break;
            case Suppression::ExcludedApplication:
            case Suppression::ExcludedTitle: ++counters.excluded; break;
            case Suppression::Unchanged: ++counters.coalesced; break;
            case Suppression::RateLimited: ++counters.rateLimited; break;
            case Suppression::Paused: break;
        }
        if (verdict == Suppression::None)
        {
            handler = observationHandler;
        }
    }
    if (handler)
    {
        handler(candidate);
    }
#else
    (void)event;
    (void)windowHandle;
#endif
}

void WindowEventMonitor::Shutdown()
{
#ifdef _WIN32
    const std::uint32_t threadId = workerThreadId.exchange(0);
    if (threadId != 0)
    {
        // Ends GetMessageW cleanly. Terminating the thread instead would leak the hooks,
        // and a leaked system-wide hook outlives the process that installed it.
        PostThreadMessageW(static_cast<DWORD>(threadId), WM_QUIT, 0, 0);
    }
#endif
    if (worker.joinable())
    {
        worker.join();
    }
    observing.store(false);
    paused.store(false);
}

void WindowEventMonitor::SetPaused(const bool value)
{
    const bool previous = paused.exchange(value);
    if (previous == value || !observing.load())
    {
        return;
    }
    if (value)
    {
        Notify("Paused", "Ambient perception is paused. Nothing is being observed.");
    }
    else
    {
        Notify("Watching", "Observing window and focus changes only. No screen capture.");
    }
}

bool WindowEventMonitor::IsPaused() const
{
    return paused.load();
}

bool WindowEventMonitor::IsObserving() const
{
    return observing.load() && !paused.load();
}

PerceptionCounters WindowEventMonitor::Counters() const
{
    std::lock_guard lock(mutex);
    return counters;
}

void WindowEventMonitor::Notify(const std::string& phase, const std::string& detail) const
{
    StatusHandler handler;
    {
        std::lock_guard lock(mutex);
        handler = statusHandler;
    }
    if (handler)
    {
        handler(phase, detail);
    }
}

} // namespace revia::perception
