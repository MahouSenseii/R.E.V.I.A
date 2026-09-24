#include "testSupport.h"

#include "Perception/screenAwarenessSchedule.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace
{

using revia::perception::ScreenAwarenessSchedule;
using revia::tests::Check;

// The scheduling half of background screen awareness, now that it can be reached.
//
// This state machine lived inside ReviaSession as six members and three methods, and it
// had no test -- not because nobody wanted one, but because exercising it meant starting
// a worker thread and a vision model. Every behaviour below was previously only
// verifiable by watching the application.

// The floor: a signal wakes the wait, and the reason travels with it.
void TestASignalWakesTheWait()
{
    ScreenAwarenessSchedule schedule;
    std::stop_source source;

    std::thread signaller([&schedule]
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        schedule.Signal("a window came forward");
    });

    const auto work = schedule.WaitForWork(
        source.get_token(), std::chrono::seconds(30), 0);
    signaller.join();

    Check(work.has_value(), "A signal did not wake the wait.");
    Check(work->eventDriven, "A signalled wake was reported as the periodic one.");
    Check(work->trigger == "a window came forward",
        "The reason did not travel with the signal; got \"" + work->trigger + "\".");
    Check(work->version == 1, "The signal version did not advance.");
}

// The timer: no signal, and the wait still returns with the periodic reason.
void TestThePeriodicRefreshStillHappens()
{
    ScreenAwarenessSchedule schedule;
    std::stop_source source;

    const auto work = schedule.WaitForWork(
        source.get_token(), std::chrono::milliseconds(60), 0);
    Check(work.has_value(), "The periodic refresh did not fire.");
    Check(!work->eventDriven, "A timed wake was reported as event-driven.");
    Check(work->trigger == "periodic multi-monitor refresh",
        "The periodic wake did not say why it woke.");
}

// A stop is not work. This is the case a caller must never treat as "time to capture".
void TestAStopIsNotWork()
{
    ScreenAwarenessSchedule schedule;
    std::stop_source source;
    source.request_stop();

    const auto work = schedule.WaitForWork(
        source.get_token(), std::chrono::seconds(30), 0);
    Check(!work.has_value(),
        "A stopped worker was handed work, which would capture during shutdown.");
}

// Several signals before the worker wakes are one wake, not several.
void TestSignalsCoalesce()
{
    ScreenAwarenessSchedule schedule;
    std::stop_source source;

    schedule.Signal("first");
    schedule.Signal("second");
    schedule.Signal("third");

    const auto work = schedule.WaitForWork(
        source.get_token(), std::chrono::seconds(30), 0);
    Check(work.has_value(), "The signals did not wake the wait.");
    Check(work->trigger == "third",
        "A coalesced wake reported a superseded reason: \"" + work->trigger + "\".");
    Check(work->version == 3,
        "The version did not count every signal, so the next wait would fire again.");

    // And having handled version 3, the wait no longer fires for it.
    const auto again = schedule.WaitForWork(
        source.get_token(), std::chrono::milliseconds(40), work->version);
    Check(again.has_value() && !again->eventDriven,
        "An already-handled signal woke the wait a second time.");
}

// The debounce settles, and the ceiling is what stops it settling forever.
void TestTheDebounceHasACeiling()
{
    ScreenAwarenessSchedule schedule;
    std::stop_source source;
    schedule.Signal("a title that keeps changing");

    // A stream of signals that never stops. Without a ceiling this waits for quiet that
    // never comes -- which is the failure the ceiling exists for, and it happens
    // precisely while somebody is actively working.
    std::atomic<bool> keepGoing{true};
    std::thread noisy([&schedule, &keepGoing]
    {
        while (keepGoing.load())
        {
            schedule.Signal("still changing");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    const auto started = std::chrono::steady_clock::now();
    const std::uint64_t settled = schedule.Settle(
        source.get_token(), std::chrono::milliseconds(50),
        std::chrono::milliseconds(300), 1);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    keepGoing.store(false);
    noisy.join();

    Check(elapsed < 1200,
        "The debounce never gave up on a screen that never settles; it waited " +
            std::to_string(elapsed) + "ms.");
    Check(settled > 1,
        "The settle did not take the newest signal with it, so the next wait would "
        "immediately fire for one already handled.");
}

// A quiet screen settles quickly rather than waiting out the ceiling.
void TestAQuietScreenSettlesEarly()
{
    ScreenAwarenessSchedule schedule;
    std::stop_source source;
    schedule.Signal("one change");

    const auto started = std::chrono::steady_clock::now();
    static_cast<void>(schedule.Settle(
        source.get_token(), std::chrono::milliseconds(40),
        std::chrono::milliseconds(2000), 1));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    Check(elapsed < 500,
        "A screen that stopped changing still waited out the whole ceiling: " +
            std::to_string(elapsed) + "ms.");
}

// Each attempt gets its own cancellation. A reused source would leave every attempt
// after the first cancelled from the moment it began.
void TestEachAttemptIsSeparatelyCancellable()
{
    ScreenAwarenessSchedule schedule;

    const auto first = schedule.BeginAttempt("first look", 0);
    Check(!first.token.stop_requested(), "A fresh attempt began already cancelled.");
    schedule.CancelAttempt();
    Check(first.token.stop_requested(), "Cancelling did not reach the attempt.");

    const auto second = schedule.BeginAttempt("second look", 0);
    Check(!second.token.stop_requested(),
        "A new attempt inherited the previous cancellation, which would stop every "
        "capture after the first user turn.");
}

// An attempt takes the newest signal with it rather than capturing a stale reason.
void TestAnAttemptTakesTheNewestSignal()
{
    ScreenAwarenessSchedule schedule;
    schedule.Signal("the old reason");
    schedule.Signal("the new reason");

    const auto attempt = schedule.BeginAttempt("the reason the worker woke with", 1);
    Check(attempt.trigger == "the new reason",
        "The attempt captured under a superseded reason: \"" + attempt.trigger + "\".");
    Check(attempt.version == 2,
        "The attempt did not absorb the newer signal, so it would be captured twice.");
}

// The age is in the text, because a model told what is on screen without being told
// when will speak about it in the present tense.
void TestTheCachedContextStatesItsAge()
{
    ScreenAwarenessSchedule schedule;
    Check(schedule.CurrentContext().empty() && !schedule.HasContext(),
        "An unobserved screen produced a context anyway.");

    schedule.Record("Two editors and a terminal are open.");
    Check(schedule.HasContext(), "A recorded observation was not kept.");
    const std::string context = schedule.CurrentContext();
    Check(context.find("Two editors and a terminal are open.") != std::string::npos,
        "The observation is not in the context.");
    Check(context.find("seconds ago") != std::string::npos,
        "The context does not say how old it is, so it reads as what is on screen now.");
    Check(context.find("untrusted content") != std::string::npos,
        "The context stopped saying that screen text is not instructions.");
}

// Comparing two looks means comparing what was seen. The framed context states its own
// age, so the same observation read a second later compared unequal, and every look
// was reported as a change on the desktop.
void TestTheSameSightComparesEqual()
{
    ScreenAwarenessSchedule schedule;
    schedule.Record("A card game on the left, an editor on the right.");
    const auto first = schedule.LatestObservation();
    Check(first.text == "A card game on the left, an editor on the right.",
        "The raw observation carried framing or lost its text.");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    Check(schedule.LatestObservation().text == first.text &&
        schedule.LatestObservation().at == first.at,
        "Reading the same observation twice made it look different.");

    schedule.Record("A card game on the left, an editor on the right.");
    const auto second = schedule.LatestObservation();
    Check(second.at != first.at && second.text == first.text,
        "A fresh look that saw the same thing could not be told apart from no look.");
}

// No observation is a truer answer than an old one presented as current.
void TestAnEmptyObservationClearsRatherThanKeeping()
{
    ScreenAwarenessSchedule schedule;
    schedule.Record("something was here");
    Check(schedule.HasContext(), "The first observation was not kept.");

    schedule.Record("");
    Check(!schedule.HasContext() && schedule.CurrentContext().empty(),
        "An attempt that saw nothing left the previous description in place, which "
        "would then be offered as the current screen.");

    schedule.Record("something again");
    schedule.Clear();
    Check(!schedule.HasContext(),
        "Clearing the observation history left the observation behind.");
}

} // namespace

void RunScreenAwarenessScheduleTests()
{
    TestASignalWakesTheWait();
    TestThePeriodicRefreshStillHappens();
    TestAStopIsNotWork();
    TestSignalsCoalesce();
    TestTheDebounceHasACeiling();
    TestAQuietScreenSettlesEarly();
    TestEachAttemptIsSeparatelyCancellable();
    TestAnAttemptTakesTheNewestSignal();
    TestTheCachedContextStatesItsAge();
    TestAnEmptyObservationClearsRatherThanKeeping();
    TestTheSameSightComparesEqual();

    std::cout << "Screen awareness coalesces, settles with a ceiling, cancels per "
                 "attempt, and never offers a stale look as a current one.\n";
}
