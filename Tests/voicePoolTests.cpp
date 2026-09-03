#include "testSupport.h"

#include "Speech/qwenTtsPool.h"

#include <chrono>
#include <future>
#include <iostream>
#include <vector>

namespace revia::speech
{

// Declared a friend by QwenTtsPool. Everything here reads or nudges scheduling state
// that a real run would get from a Python worker finishing a phrase.
struct VoicePoolTestAccess
{
    static void Estimate(
        QwenTtsPool& pool,
        const std::size_t index,
        const double fixedOverheadMilliseconds,
        const double millisecondsPerCharacter)
    {
        std::lock_guard lock(pool.mutex);
        pool.workers[index].fixedOverheadMilliseconds = fixedOverheadMilliseconds;
        pool.workers[index].millisecondsPerCharacter = millisecondsPerCharacter;
    }

    static std::size_t Acquire(
        QwenTtsPool& pool,
        const std::size_t characters,
        const bool latencyCritical,
        double& waitMilliseconds)
    {
        return pool.AcquireWorker(characters, latencyCritical, waitMilliseconds);
    }

    static void Release(
        QwenTtsPool& pool,
        const std::size_t index,
        const std::size_t characters,
        const double milliseconds)
    {
        pool.ReleaseWorker(index, characters, milliseconds);
    }

    static bool Busy(QwenTtsPool& pool, const std::size_t index)
    {
        std::lock_guard lock(pool.mutex);
        return pool.workers[index].busy;
    }
};

} // namespace revia::speech

namespace
{
using revia::tests::Check;
using revia::speech::QwenTtsPool;
using revia::speech::SelectIdleVoiceWorker;
using revia::speech::VoicePoolTestAccess;
using revia::speech::VoiceWorkerState;

// A pool of two workers with no Python behind them. Configure only builds client
// objects and stores settings, so this costs nothing and starts nothing.
speechSettings TwoDeviceSettings()
{
    speechSettings settings;
    settings.qwenDevice = "cuda:0";
    settings.qwenDevices = {"cuda:0", "cuda:1"};
    settings.qwenMaxWorkers = 2;
    return settings;
}

// The regression. Worker 0 is faster per character and busy; worker 1 is slower and
// free. The previous scheduler compared the two on predicted finish time, chose the
// busy one, and then waited for it -- so a card sat idle while phrases queued, and
// every other caller made the same choice for the same reason.
void TestAnIdleWorkerIsNeverPassedOverForABusyOne()
{
    std::vector<VoiceWorkerState> workers(2);
    workers[0] = {true, 1000.0, 10.0};
    workers[1] = {false, 4000.0, 90.0};

    Check(SelectIdleVoiceWorker(workers, 64, false) == 1,
        "A busy worker was chosen over an idle one because it was historically faster.");
    Check(SelectIdleVoiceWorker(workers, 64, true) == 1,
        "The first phrase waited on a busy worker 0 while another worker was free.");
}

void TestTheBestIdleWorkerWinsWhenBothAreFree()
{
    std::vector<VoiceWorkerState> workers(2);
    workers[0] = {false, 4000.0, 90.0};
    workers[1] = {false, 1000.0, 10.0};

    Check(SelectIdleVoiceWorker(workers, 64, false) == 1,
        "The slower of two idle workers was chosen.");
    // Latency-critical still prefers worker 0 while it is free: that is the resource
    // planner's placement for the phrase the listener is waiting on, and a preference
    // is not the same thing as the pin that was removed.
    Check(SelectIdleVoiceWorker(workers, 64, true) == 0,
        "The latency-first placement was ignored while worker 0 was free.");
}

void TestEveryWorkerBusyMeansWait()
{
    std::vector<VoiceWorkerState> workers(2);
    workers[0] = {true, 1000.0, 10.0};
    workers[1] = {true, 4000.0, 90.0};

    Check(SelectIdleVoiceWorker(workers, 64, false) == workers.size(),
        "A busy pool handed out a worker that was already working.");
    Check(SelectIdleVoiceWorker(workers, 64, true) == workers.size(),
        "A busy pool handed out worker 0 to a latency-critical caller.");
    Check(SelectIdleVoiceWorker({}, 64, false) == 0,
        "An empty pool did not report that there is nothing to acquire.");
}

// Characters matter only between idle workers, and only through the per-character
// term. A long phrase can reverse the choice a short one made.
void TestPhraseLengthDecidesBetweenIdleWorkers()
{
    std::vector<VoiceWorkerState> workers(2);
    workers[0] = {false, 500.0, 90.0};
    workers[1] = {false, 4000.0, 10.0};

    Check(SelectIdleVoiceWorker(workers, 8, false) == 0,
        "A short phrase ignored the worker with the lower fixed overhead.");
    Check(SelectIdleVoiceWorker(workers, 400, false) == 1,
        "A long phrase ignored the worker with the lower per-character cost.");
}

// The same property through the live pool, with the real mutex and the real state.
void TestThePoolDispatchesToAFreeWorkerWithoutWaiting()
{
    QwenTtsPool pool;
    pool.Configure(TwoDeviceSettings());
    Check(pool.WorkerCount() == 2, "The two-device pool did not build two workers.");
    VoicePoolTestAccess::Estimate(pool, 0, 1000.0, 10.0);
    VoicePoolTestAccess::Estimate(pool, 1, 9000.0, 400.0);

    double firstWait = -1.0;
    const std::size_t first = VoicePoolTestAccess::Acquire(pool, 64, true, firstWait);
    Check(first == 0, "The latency-first worker was not chosen while it was free.");

    // Worker 0 is now busy and still looks far cheaper than worker 1. The call must
    // return worker 1 rather than block, and it must do so while 0 is still held.
    double secondWait = -1.0;
    const std::size_t second = VoicePoolTestAccess::Acquire(pool, 64, false, secondWait);
    Check(second == 1, "A second phrase blocked on a busy worker while one was idle.");
    Check(VoicePoolTestAccess::Busy(pool, 0),
        "The first worker was released before the second was acquired.");
    Check(secondWait >= 0.0, "The pool wait was not measured.");

    VoicePoolTestAccess::Release(pool, 0, 64, 900.0);
    VoicePoolTestAccess::Release(pool, 1, 64, 5000.0);
}

// Waiting is correct when there is genuinely nothing free, and the choice made on
// waking has to come from the state as it is then. A caller that remembered the
// worker it wanted before the wait would be answering a question about a pool that no
// longer exists.
void TestAWaitingCallerWakesAndRecomputes()
{
    QwenTtsPool pool;
    pool.Configure(TwoDeviceSettings());
    VoicePoolTestAccess::Estimate(pool, 0, 1000.0, 10.0);
    VoicePoolTestAccess::Estimate(pool, 1, 1200.0, 12.0);

    double firstWait = -1.0;
    double secondWait = -1.0;
    Check(VoicePoolTestAccess::Acquire(pool, 64, false, firstWait) == 0,
        "The cheaper idle worker was not chosen first.");
    Check(VoicePoolTestAccess::Acquire(pool, 64, false, secondWait) == 1,
        "The second idle worker was not chosen while it was free.");

    std::promise<std::size_t> acquired;
    std::future<std::size_t> waitedFor = acquired.get_future();
    double waitingCallerWait = -1.0;
    std::thread waiter([&]
    {
        acquired.set_value(
            VoicePoolTestAccess::Acquire(pool, 64, false, waitingCallerWait));
    });

    // Nothing is free, so the caller must still be inside AcquireWorker.
    Check(waitedFor.wait_for(std::chrono::milliseconds(120)) ==
            std::future_status::timeout,
        "A caller was handed a worker while every worker was busy.");

    // Free the worker the waiter did not originally have any claim on. It must take
    // this one, which is only possible if the choice was made after waking.
    VoicePoolTestAccess::Release(pool, 1, 64, 1500.0);
    Check(waitedFor.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "A waiting caller was not woken when a worker was released.");
    const std::size_t woke = waitedFor.get();
    waiter.join();
    Check(woke == 1, "The woken caller did not take the worker that became free.");
    Check(waitingCallerWait > 0.0,
        "A caller that blocked reported no pool wait at all.");

    VoicePoolTestAccess::Release(pool, 0, 64, 900.0);
    VoicePoolTestAccess::Release(pool, 1, 64, 1500.0);
}

// Shutdown has to release a blocked caller rather than leave it holding the loop.
void TestShutdownReleasesAWaitingCaller()
{
    QwenTtsPool pool;
    pool.Configure(TwoDeviceSettings());
    double firstWait = -1.0;
    double secondWait = -1.0;
    VoicePoolTestAccess::Acquire(pool, 64, false, firstWait);
    VoicePoolTestAccess::Acquire(pool, 64, false, secondWait);

    std::promise<std::size_t> acquired;
    std::future<std::size_t> waitedFor = acquired.get_future();
    double waitingCallerWait = -1.0;
    std::thread waiter([&]
    {
        acquired.set_value(
            VoicePoolTestAccess::Acquire(pool, 64, false, waitingCallerWait));
    });
    Check(waitedFor.wait_for(std::chrono::milliseconds(120)) ==
            std::future_status::timeout,
        "A caller was handed a worker while every worker was busy.");

    pool.RequestShutdown();
    Check(waitedFor.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "Shutdown left a caller waiting for a worker that will never come.");
    Check(waitedFor.get() == pool.WorkerCount(),
        "A shutting-down pool handed out a worker instead of refusing.");
    waiter.join();
    Check(waitingCallerWait >= 0.0,
        "A caller that waited and then got nothing reported no wait.");

    VoicePoolTestAccess::Release(pool, 0, 64, 900.0);
    VoicePoolTestAccess::Release(pool, 1, 64, 900.0);
}

} // namespace

void RunVoicePoolTests()
{
    TestAnIdleWorkerIsNeverPassedOverForABusyOne();
    TestTheBestIdleWorkerWinsWhenBothAreFree();
    TestEveryWorkerBusyMeansWait();
    TestPhraseLengthDecidesBetweenIdleWorkers();
    TestThePoolDispatchesToAFreeWorkerWithoutWaiting();
    TestAWaitingCallerWakesAndRecomputes();
    TestShutdownReleasesAWaitingCaller();
    std::cout << "The voice pool dispatches to an idle worker rather than waiting on a "
                 "busy one, decides between idle workers on\npredicted cost, and "
                 "recomputes that choice after every wait.\n";
}
