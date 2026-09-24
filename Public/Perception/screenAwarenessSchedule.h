#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

namespace revia::perception
{

// When background screen awareness should look, and what it last saw.
//
// Extracted from ReviaSession, which owned six members and three methods of this and
// nothing else in the file touched them. What stayed behind is the *capture*: that work
// is interleaved with session lifecycle -- it takes the foreground operation lock,
// yields to a user turn, and restarts the vision backend -- and pulling it out behind a
// set of callbacks would be a forwarding layer wearing an owner's name.
//
// What moved is the part that is genuinely its own: a signal with a version, a debounce
// with a ceiling, a cancellable attempt, and the cached result with its age. That state
// machine had no test, because reaching it meant starting a thread and a vision model.
// It has one now.
//
// Thread-safe. The signal arrives on whatever noticed the window change, the wait
// happens on the awareness worker, and the cached context is read by whichever turn is
// building a prompt.
class ScreenAwarenessSchedule
{
public:
    // Why the worker woke, and which signal it is answering.
    struct Work
    {
        std::string trigger;
        std::uint64_t version = 0;
        // True when a signal woke it rather than the periodic timer. Only an
        // event-driven wake debounces: a scheduled refresh has nothing to settle.
        bool eventDriven = false;
    };

    // Ask for a look. Coalesces: a second signal before the worker wakes replaces the
    // reason and bumps the version, rather than queueing a second capture.
    void Signal(std::string reason);

    // Stop the attempt in flight. Called when a user turn arrives, because background
    // vision must never be the reason a person waits.
    void CancelAttempt();

    // Wait until there is something to do, or until the refresh interval elapses.
    //
    // Returns nothing when the worker was asked to stop, which is the one case a caller
    // must not treat as "time to capture".
    [[nodiscard]] std::optional<Work> WaitForWork(
        std::stop_token workerStop,
        std::chrono::milliseconds refreshInterval,
        std::uint64_t handledVersion);

    // Wait for the screen to settle after an event, up to a ceiling.
    //
    // The ceiling is the point. Editors and terminals change their title continuously,
    // and a debounce that only waited for quiet would postpone awareness for as long as
    // somebody kept typing -- which is exactly when it is most wanted.
    //
    // Returns the version to treat as handled: a signal arriving during the wait
    // supersedes the one being settled rather than scheduling a second capture.
    [[nodiscard]] std::uint64_t Settle(
        std::stop_token workerStop,
        std::chrono::milliseconds debounce,
        std::chrono::milliseconds ceiling,
        std::uint64_t targetVersion);

    // Begin one cancellable attempt, and take the newest signal with it.
    struct Attempt
    {
        std::stop_token token;
        std::string trigger;
        std::uint64_t version = 0;
    };
    [[nodiscard]] Attempt BeginAttempt(std::string trigger, std::uint64_t targetVersion);

    // What the attempt saw. An empty description clears the cache rather than leaving a
    // stale one behind: no observation is a truer answer than an old one presented as
    // current.
    void Record(std::string description);

    // The cached observation, with its age stated in the text.
    //
    // The age is not decoration. A model told what is on screen without being told when
    // will speak about it in the present tense, and the one thing this must never do is
    // let an observation from four minutes ago be read as what is there now.
    [[nodiscard]] std::string CurrentContext() const;
    // Just what was seen and when, without the framing. For comparing one look with the
    // next: CurrentContext() states its own age, so two copies of the same observation
    // read a second apart never compare equal.
    struct Observation
    {
        std::string text;
        std::chrono::steady_clock::time_point at{};
    };
    [[nodiscard]] Observation LatestObservation() const;

    // Sleep until a moment, or until asked to stop. For the minimum-interval floor,
    // which is a wait about the clock rather than about a signal.
    //
    // Returns false when the worker was asked to stop.
    [[nodiscard]] bool WaitUntil(
        std::stop_token workerStop,
        std::chrono::steady_clock::time_point until);

    // Whether anything has been observed yet.
    [[nodiscard]] bool HasContext() const;

    // Drops the cached observation. For a stop, where leaving a description behind
    // would outlive the awareness that justified keeping it.
    void Clear();

private:
    mutable std::mutex mutex;
    std::condition_variable_any condition;
    std::uint64_t signalVersion = 0;
    std::string signalReason;
    std::stop_source attemptStopSource;
    std::string latestContext;
    std::chrono::steady_clock::time_point latestContextAt{};
};

} // namespace revia::perception
