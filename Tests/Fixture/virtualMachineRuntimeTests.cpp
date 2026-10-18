#include "Workspace/virtualMachineRuntime.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace revia::workspace;
using namespace std::chrono_literals;

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool WaitFor(const std::function<bool()>& condition)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (condition()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}

class Provider final : public IVirtualMachineProvider
{
  public:
    std::atomic<int> ensures{0}, executions{0}, stops{0};
    bool holdSetup = false, holdExecution = false, throwSetup = false, throwExecution = false, wrongBoot = false;
    bool failProcess = false;
    bool omitProof = false;
    std::function<void()> onCancellation;
    VirtualMachineSnapshot EnsureReady(std::stop_token stop) override
    {
        ++ensures;
        if (throwSetup) throw std::runtime_error("private provisioning secret");
        if (holdSetup) Check(WaitFor([&] { return stop.stop_requested(); }), "Setup did not receive cancellation.");
        return {VirtualMachineState::Ready, "ready", "Guest ready", {"environment", "boot"}};
    }
    GuestResult Execute(const GuestRequest& request, const GuestLimits&, std::stop_token stop) override
    {
        std::stop_callback notify(stop, [&] { if (onCancellation) onCancellation(); });
        ++executions;
        if (throwExecution) throw 19;
        if (holdExecution) Check(WaitFor([&] { return stop.stop_requested(); }), "Execution did not receive cancellation.");
        GuestResult result;
        result.attempted = true;
        result.cancelled = stop.stop_requested();
        result.succeeded = !result.cancelled;
        result.receiptVerified = !omitProof;
        result.identity = {"environment", wrongBoot ? "restarted" : "boot"};
        if (failProcess && request.operation == GuestOperation::ExecuteProcess)
        {
            result.succeeded = false;
            result.uncertainEffect = true;
            result.receiptVerified = true;
            result.exitCode = 1;
        }
        return result;
    }
    GuestResult StopGuest() override
    {
        ++stops;
        GuestResult result;
        result.succeeded = true;
        return result;
    }
};

void Ready(VirtualMachineRuntime& runtime)
{
    runtime.Start();
    Check(WaitFor([&] { return runtime.Snapshot().state == VirtualMachineState::Ready; }),
        "Startup never admitted a verified guest readiness result.");
}

GuestRequest Request()
{
    GuestRequest request;
    request.operation = GuestOperation::WriteText;
    request.path = "example.txt";
    request.content = "hello";
    request.expectedDigest = "missing";
    request.expectedIdentity = {"environment", "boot"};
    return request;
}

void TestReadinessAndIdentity()
{
    auto provider = std::make_shared<Provider>();
    VirtualMachineRuntime runtime(provider);
    Check(!runtime.Execute(Request(), {}).attempted && provider->executions == 0, "Disabled VM accepted a request.");
    Ready(runtime);
    runtime.Start();
    Check(provider->ensures == 1, "Repeated startup provisioned a second VM.");
    auto stale = Request();
    stale.expectedIdentity.bootId = "old-boot";
    Check(!runtime.Execute(stale, {}).attempted && provider->executions == 0, "A stale boot plan reached the provider.");
    stale.expectedIdentity = {};
    Check(!runtime.Execute(stale, {}).attempted && provider->executions == 0, "An unstamped guest request was admitted.");
    Check(runtime.Execute(Request(), {}).succeeded && provider->executions == 1, "The admitted guest request did not execute.");
    runtime.Stop();
    runtime.Stop();
    Check(provider->stops == 1 && runtime.Snapshot().state == VirtualMachineState::Stopped,
        "Owned guest shutdown was not idempotent.");
}

void TestCancelStartup()
{
    auto provider = std::make_shared<Provider>();
    provider->holdSetup = true;
    VirtualMachineRuntime runtime(provider);
    runtime.Start();
    Check(WaitFor([&] { return provider->ensures == 1; }), "Setup did not start asynchronously.");
    runtime.Stop();
    Check(runtime.Snapshot().state == VirtualMachineState::Stopped && provider->stops == 1,
        "Late readiness survived startup cancellation.");
}

void TestPreparingGuestRefusesWithoutWaitingForSetup()
{
    auto provider = std::make_shared<Provider>();
    provider->holdSetup = true;
    VirtualMachineRuntime runtime(provider);
    runtime.Start();
    Check(WaitFor([&] { return provider->ensures == 1; }), "Setup did not enter its held phase.");
    auto execution = std::async(std::launch::async, [&] { return runtime.Execute(Request(), {}); });
    const bool responsive = execution.wait_for(100ms) == std::future_status::ready;
    runtime.Stop();
    const auto result = execution.get();
    Check(responsive && !result.attempted && provider->executions == 0,
        "A request waited for long-running VM setup instead of reporting not ready.");
}

void TestProviderFailures()
{
    auto provider = std::make_shared<Provider>();
    provider->throwSetup = true;
    VirtualMachineRuntime runtime(provider);
    runtime.Start();
    Check(WaitFor([&] { return runtime.Snapshot().state == VirtualMachineState::Blocked; }),
        "Provisioning exception was not reported as blocked.");
    Check(runtime.Snapshot().message.find("private") == std::string::npos, "Provisioning leaked exception content.");
    provider->throwSetup = false;
    Ready(runtime);
    provider->throwExecution = true;
    const auto result = runtime.Execute(Request(), {});
    Check(result.attempted && !result.succeeded && result.uncertainEffect,
        "A provider exception lost uncertainty about a submitted effect.");
    Check(runtime.Snapshot().state == VirtualMachineState::Blocked && !runtime.Execute(Request(), {}).attempted &&
        provider->executions == 1, "A provider exception permitted dependent guest work.");
}

void TestShutdownCancelsExecution()
{
    auto provider = std::make_shared<Provider>();
    provider->holdExecution = true;
    VirtualMachineRuntime runtime(provider);
    Ready(runtime);
    auto result = std::async(std::launch::async, [&] { return runtime.Execute(Request(), {}); });
    Check(WaitFor([&] { return provider->executions == 1; }), "Guest execution did not start.");
    runtime.Stop();
    Check(result.get().cancelled && provider->stops == 1, "Shutdown did not cancel execution before guest stop.");
}

void TestReceiptAndNotificationFailure()
{
    auto provider = std::make_shared<Provider>();
    VirtualMachineRuntime runtime(provider);
    runtime.SetObserver([](const VirtualMachineSnapshot&) { throw 4; });
    Ready(runtime);
    Check(runtime.Snapshot().notificationFailed, "Status observer failure was hidden.");
    provider->wrongBoot = true;
    const auto result = runtime.Execute(Request(), {});
    Check(!result.succeeded && result.uncertainEffect && runtime.Snapshot().state == VirtualMachineState::Blocked,
        "A receipt from another boot was accepted as current execution proof.");
}

void TestCancellationCallbackCanInspectStatus()
{
    auto provider = std::make_shared<Provider>();
    provider->holdExecution = true;
    VirtualMachineRuntime runtime(provider);
    std::atomic<bool> inspected{false};
    bool callbackCouldInspect = false;
    std::thread reader;
    provider->onCancellation = [&]
    {
        reader = std::thread([&] { (void)runtime.Snapshot(); inspected = true; });
        const auto deadline = std::chrono::steady_clock::now() + 100ms;
        while (!inspected && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        callbackCouldInspect = inspected;
    };
    Ready(runtime);
    auto result = std::async(std::launch::async, [&] { return runtime.Execute(Request(), {}); });
    Check(WaitFor([&] { return provider->executions == 1; }), "Guest operation did not start.");
    runtime.Stop();
    if (reader.joinable()) reader.join();
    Check(result.get().cancelled && callbackCouldInspect,
        "Shutdown invoked a provider cancellation callback while holding the status lock.");
}

void TestFailedTestStillAllowsDiagnosis()
{
    auto provider = std::make_shared<Provider>();
    provider->failProcess = true;
    VirtualMachineRuntime runtime(provider);
    Ready(runtime);
    auto request = Request();
    request.operation = GuestOperation::ExecuteProcess;
    const auto failure = runtime.Execute(request, {});
    Check(!failure.succeeded && failure.receiptVerified && failure.exitCode == 1 && provider->executions == 1,
        "A failed guest test was lost or repeated.");
    request.operation = GuestOperation::ReadText;
    const auto diagnosis = runtime.Execute(request, {});
    Check(diagnosis.succeeded && provider->executions == 2,
        "A verified failing guest test blocked subsequent read-only diagnosis.");
}

void TestUnverifiedSuccessIsNotAccepted()
{
    auto provider = std::make_shared<Provider>();
    provider->omitProof = true;
    VirtualMachineRuntime runtime(provider);
    Ready(runtime);
    const auto result = runtime.Execute(Request(), {});
    Check(!result.succeeded && result.uncertainEffect && runtime.Snapshot().state == VirtualMachineState::Blocked,
        "A success claim without verified receipt was admitted.");
}

void TestStartupWaitsForPrerequisite()
{
    auto provider = std::make_shared<Provider>();
    VirtualMachineRuntime runtime(provider);
    std::atomic<bool> entered{false}, released{false};
    runtime.Start([&](std::stop_token stop)
    {
        entered = true;
        while (!released && !stop.stop_requested()) std::this_thread::sleep_for(1ms);
        return released.load();
    });
    const bool waiting = WaitFor([&] { return entered.load() || provider->ensures > 0; });
    const bool gated = waiting && entered && provider->ensures == 0 &&
        runtime.Snapshot().state == VirtualMachineState::Preparing && !runtime.Execute(Request(), {}).attempted;
    released = true;
    const bool ready = WaitFor([&] { return runtime.Snapshot().state == VirtualMachineState::Ready; });
    runtime.Stop();
    Check(gated && ready && provider->ensures == 1 && provider->stops == 1,
        "VM provisioning did not wait asynchronously for its host prerequisite.");
}

void TestCancelPrerequisiteDoesNotTouchProvider()
{
    auto provider = std::make_shared<Provider>();
    VirtualMachineRuntime runtime(provider);
    std::atomic<bool> entered{false}, cancelled{false};
    runtime.Start([&](std::stop_token stop)
    {
        entered = true;
        while (!stop.stop_requested()) std::this_thread::sleep_for(1ms);
        cancelled = true;
        return true; // A late completion must not start the provider after cancellation.
    });
    Check(WaitFor([&] { return entered.load() || provider->ensures > 0; }), "The startup gate did not begin.");
    const auto before = std::chrono::steady_clock::now();
    runtime.Stop();
    Check(cancelled && std::chrono::steady_clock::now() - before < 500ms && provider->ensures == 0 && provider->stops == 0 &&
        runtime.Snapshot().state == VirtualMachineState::Stopped,
        "Cancelling a host prerequisite entered or stopped a provider it had not started.");
}

void TestPrerequisiteFailuresAreContained()
{
    for (const bool throws : {false, true})
    {
        auto provider = std::make_shared<Provider>();
        VirtualMachineRuntime runtime(provider);
        runtime.Start([throws](std::stop_token) -> bool
        {
            if (throws) throw std::runtime_error("private host startup detail");
            return false;
        });
        Check(WaitFor([&] { return runtime.Snapshot().state != VirtualMachineState::Preparing; }),
            "A failed host prerequisite left startup pending.");
        const auto failed = runtime.Snapshot();
        runtime.Stop();
        Check(failed.state == VirtualMachineState::Blocked && failed.code == "startup_prerequisite_failed" &&
            failed.message.find("private host") == std::string::npos && provider->ensures == 0 && provider->stops == 0,
            "A failed host prerequisite entered the provider or leaked its exception.");
    }
}

void TestFailedRetryStillStopsPreviouslyEnteredProvider()
{
    auto provider = std::make_shared<Provider>();
    VirtualMachineRuntime runtime(provider);
    Ready(runtime);
    provider->throwExecution = true;
    (void)runtime.Execute(Request(), {});
    runtime.Start([](std::stop_token) { return false; });
    Check(WaitFor([&] { return runtime.Snapshot().state == VirtualMachineState::Blocked; }),
        "The retry prerequisite did not fail.");
    runtime.Stop();
    Check(provider->ensures == 1 && provider->stops == 1,
        "A failed retry forgot the provider entered by its earlier startup.");
}
}

int main()
{
    try
    {
        TestReadinessAndIdentity();
        TestCancelStartup();
        TestPreparingGuestRefusesWithoutWaitingForSetup();
        TestProviderFailures();
        TestShutdownCancelsExecution();
        TestReceiptAndNotificationFailure();
        TestCancellationCallbackCanInspectStatus();
        TestFailedTestStillAllowsDiagnosis();
        TestUnverifiedSuccessIsNotAccepted();
        TestStartupWaitsForPrerequisite();
        TestCancelPrerequisiteDoesNotTouchProvider();
        TestPrerequisiteFailuresAreContained();
        TestFailedRetryStillStopsPreviouslyEnteredProvider();
        std::cout << "VM lifecycle: 13 tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
