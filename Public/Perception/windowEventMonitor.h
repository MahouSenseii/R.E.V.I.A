#pragma once

#include "Library/structLibrary.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace revia::perception
{

enum class ObservationKind
{
    ForegroundChanged,
    WindowOpened,
    TitleChanged
};

// One structured fact about what is in front. Deliberately not a frame, not pixels, and
// not text from inside the window.
struct WindowObservation
{
    ObservationKind kind = ObservationKind::ForegroundChanged;
    std::string application;
    std::string windowTitle;
    std::chrono::system_clock::time_point occurredAt = std::chrono::system_clock::now();
};

[[nodiscard]] std::string ToString(ObservationKind value);

// Why an event produced no observation. Counted rather than logged with its subject, so
// suppression is measurable without the suppressed content being written anywhere.
enum class Suppression
{
    None,
    Paused,
    ExcludedApplication,
    ExcludedTitle,
    Unchanged,
    RateLimited
};

struct PerceptionCounters
{
    std::uint64_t observed = 0;
    std::uint64_t excluded = 0;
    std::uint64_t coalesced = 0;
    std::uint64_t rateLimited = 0;
};

// The admission rule, separated from the hook so it can be tested directly. Every event
// passes through here before becoming an observation, and it is the only place that
// decides whether something is recorded.
class PerceptionFilter
{
public:
    explicit PerceptionFilter(perceptionSettings settings);

    // Advances internal state (last-seen window, rate budget) only when it admits.
    [[nodiscard]] Suppression Admit(
        const WindowObservation& candidate,
        std::chrono::steady_clock::time_point now);

    [[nodiscard]] static bool IsExcludedApplication(
        const perceptionSettings& settings,
        const std::string& application);
    [[nodiscard]] static bool IsExcludedTitle(
        const perceptionSettings& settings,
        const std::string& windowTitle);

private:
    perceptionSettings configuration;
    std::string lastApplication;
    std::string lastTitle;
    std::chrono::steady_clock::time_point lastAdmitted{};
    std::chrono::steady_clock::time_point windowStarted{};
    std::uint32_t windowCount = 0;
    bool hasAdmitted = false;
};

// Tier 0 of the perception stack: foreground changes, window creation, and title changes
// via SetWinEventHook. Event-driven, so it costs nothing while the desktop is idle.
//
// The hook runs on its own thread with its own message pump. WINEVENT_OUTOFCONTEXT
// delivers callbacks through the installing thread's message queue, so without a pump on
// a thread Revia owns, the events would either never arrive or would land on the UI
// thread and couple perception to the Qt window being open.
class WindowEventMonitor
{
public:
    using ObservationHandler = std::function<void(const WindowObservation&)>;
    using StatusHandler = std::function<void(const std::string& phase, const std::string& detail)>;

    WindowEventMonitor() = default;
    ~WindowEventMonitor();

    WindowEventMonitor(const WindowEventMonitor&) = delete;
    WindowEventMonitor& operator=(const WindowEventMonitor&) = delete;

    bool Start(
        const perceptionSettings& settings,
        ObservationHandler observationHandler,
        StatusHandler statusHandler);
    void Shutdown();

    // Stops observing without stopping Revia. The hook stays installed and the thread
    // stays alive; events are dropped at the filter, so resuming is immediate.
    void SetPaused(bool paused);
    [[nodiscard]] bool IsPaused() const;
    [[nodiscard]] bool IsObserving() const;
    [[nodiscard]] PerceptionCounters Counters() const;

    // Public only because the SetWinEventHook callback is a free function with a fixed
    // signature and cannot be a member. Not part of the intended interface.
    void HandleRawEvent(std::uint32_t event, void* windowHandle);

private:
    void Run();
    void Notify(const std::string& phase, const std::string& detail) const;

    mutable std::mutex mutex;
    perceptionSettings configuration;
    ObservationHandler observationHandler;
    StatusHandler statusHandler;
    PerceptionFilter filter{perceptionSettings{}};
    PerceptionCounters counters;
    std::jthread worker;
    std::atomic<bool> observing = false;
    std::atomic<bool> paused = false;
    std::atomic<std::uint32_t> workerThreadId = 0;
};

} // namespace revia::perception
