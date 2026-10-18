#include "reviaSessionTestAccess.h"
#include "Workspace/virtualMachineRuntime.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

namespace
{
using namespace revia;
using namespace std::chrono_literals;
using Access = runtime::ReviaSessionTestAccess;
using tests::Check;

bool Until(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    return predicate();
}

class Provider final : public workspace::IVirtualMachineProvider
{
  public:
    std::atomic<unsigned> starts{0}, stops{0};
    std::atomic<bool> release{false}, cancelled{false};
    bool hold = false;
    std::string environment = "fixture-environment";
    std::string blockedCode;
    workspace::VirtualMachineSnapshot EnsureReady(std::stop_token stop) override
    {
        ++starts;
        while (hold && !release.load() && !stop.stop_requested())
            std::this_thread::sleep_for(1ms);
        cancelled.store(stop.stop_requested());
        if (!blockedCode.empty())
            return {workspace::VirtualMachineState::Blocked, blockedCode, "PRIVATE_PROVIDER_MESSAGE", {}};
        return {workspace::VirtualMachineState::Ready, "PRIVATE_PROVIDER_CODE", "PRIVATE_PROVIDER_MESSAGE", {environment, "boot"}};
    }
    workspace::GuestResult Execute(const workspace::GuestRequest&, const workspace::GuestLimits&, std::stop_token) override
    { return {}; }
    workspace::GuestResult StopGuest() override
    {
        ++stops;
        workspace::GuestResult result;
        result.succeeded = true;
        return result;
    }
};

void OptIn(const runtime::CompanionPaths& paths, const std::string& owner)
{
    const auto path = paths.Resolve("RuntimeData/VirtualMachine/settings.json");
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path);
    file << "{\"schemaVersion\":1,\"enabled\":true,\"ownerId\":\"" << owner << "\"}";
}

void TestStartupRequiresThisCompanionsOptIn()
{
    tests::ScopedTestDirectory directory;
    runtime::CompanionPaths paths(directory.root, {"vm-off", "VM off", "assistant", false});
    runtime::ReviaSession session(paths);
    auto provider = std::make_shared<Provider>();
    Access::UseVirtualMachine(session, std::make_shared<workspace::VirtualMachineRuntime>(provider));
    Access::StartVirtualMachine(session);
    Check(provider->starts == 0 && !std::filesystem::exists(paths.Resolve("RuntimeData/VirtualMachine/settings.json")),
        "Absent startup opt-in prepared a guest or created authority settings.");
    OptIn(paths, "some-other-companion");
    Access::UseVirtualMachine(session, std::make_shared<workspace::VirtualMachineRuntime>(provider));
    Access::StartVirtualMachine(session);
    Check(provider->starts == 0, "Another companion's opt-in started this companion's guest.");
}

void TestStartupKeepsConversationResponsiveAndShutdownCancels()
{
    tests::ScopedTestDirectory directory;
    runtime::CompanionPaths paths(directory.root, {"vm-background", "VM background", "assistant", false});
    OptIn(paths, paths.Descriptor().id);
    auto provider = std::make_shared<Provider>();
    provider->hold = true;
    auto machine = std::make_shared<workspace::VirtualMachineRuntime>(provider);
    runtime::ReviaSession session(paths);
    const auto identity = session.Stamp();
    Access::UseVirtualMachine(session, machine);
    const auto before = std::chrono::steady_clock::now();
    Access::StartVirtualMachine(session);
    const bool returnedPromptly = std::chrono::steady_clock::now() - before < 500ms;
    Check(Until([&] { return provider->starts == 1; }), "Enabled companion did not prepare its guest.");
    Access::MarkStudioStarted(session, true);
    const auto conversation = session.Submit("/task");
    const bool remainsPreparing = machine->Snapshot().state == workspace::VirtualMachineState::Preparing;
    Access::StartVirtualMachine(session);
    Check(returnedPromptly && conversation.succeeded && remainsPreparing && provider->starts == 1,
        "Guest preparation blocked conversation or started duplicate workers.");
    Check(session.Stamp().SameSession(identity) && !Access::Actions(session).Settings().guest.enabled,
        "Starting a VM changed Revia's identity or granted guest actions.");
    session.Stop();
    Check(provider->cancelled && provider->stops == 1 && machine->Snapshot().state == workspace::VirtualMachineState::Stopped,
        "Session shutdown did not cancel preparation and intentionally stop its guest.");
}

void TestStatusAndGuestIdentityRemainCompanionLocal()
{
    tests::ScopedTestDirectory directory;
    runtime::CompanionPaths firstPaths(directory.root, {"vm-first", "VM first", "assistant", false});
    runtime::CompanionPaths secondPaths(directory.root, {"vm-second", "VM second", "assistant", false});
    OptIn(firstPaths, firstPaths.Descriptor().id);
    OptIn(secondPaths, secondPaths.Descriptor().id);
    auto firstProvider = std::make_shared<Provider>();
    auto secondProvider = std::make_shared<Provider>();
    firstProvider->environment = "first-environment";
    secondProvider->environment = "second-environment";
    auto firstMachine = std::make_shared<workspace::VirtualMachineRuntime>(firstProvider);
    auto secondMachine = std::make_shared<workspace::VirtualMachineRuntime>(secondProvider);
    runtime::ReviaSession first(firstPaths), second(secondPaths);
    std::atomic<bool> leaked{false};
    const auto subscription = first.Events().Subscribe([&](const runtime::RuntimeEvent& event)
    {
        if (event.component == "Virtual machine" &&
            (event.message.find("PRIVATE_PROVIDER") != std::string::npos || !event.stamp.SameSession(first.Stamp())))
            leaked.store(true);
    });
    Access::UseVirtualMachine(first, firstMachine);
    Access::UseVirtualMachine(second, secondMachine);
    Access::StartVirtualMachine(first);
    Access::StartVirtualMachine(second);
    Check(Until([&] { return firstMachine->Snapshot().state == workspace::VirtualMachineState::Ready &&
        secondMachine->Snapshot().state == workspace::VirtualMachineState::Ready; }), "The two independent guests never became ready.");
    first.Stop();
    Check(!leaked && firstProvider->stops == 1 && secondProvider->stops == 0 &&
        secondMachine->Snapshot().identity.environmentId == "second-environment" && !first.Stamp().SameSession(second.Stamp()),
        "Guest status disclosed provider data, shared identity, or stopped another companion's guest.");
    first.Events().Unsubscribe(subscription);
}

void TestGuestWaitsForVoiceWithoutBlockingConversation()
{
    tests::ScopedTestDirectory directory;
    runtime::CompanionPaths paths(directory.root, {"vm-voice-wait", "VM voice wait", "assistant", false});
    OptIn(paths, paths.Descriptor().id);
    auto provider = std::make_shared<Provider>();
    auto machine = std::make_shared<workspace::VirtualMachineRuntime>(provider);
    runtime::ReviaSession session(paths);
    Access::UseVirtualMachine(session, machine);
    Access::SetVoiceWarmupSucceeded(session, false);
    Access::SetVoiceWarmupFinished(session, false);
    Access::StartVirtualMachine(session);
    Access::MarkStudioStarted(session, true);
    const auto conversation = session.Submit("/task");
    // The provider is an observable effect: it must remain untouched throughout
    // this deliberately unfinished warmup, not merely return to Preparing later.
    const bool enteredEarly = Until([&] { return provider->starts > 0; });
    const bool preparing = machine->Snapshot().state == workspace::VirtualMachineState::Preparing;
    Access::SetVoiceWarmupSucceeded(session, true);
    Access::SetVoiceWarmupFinished(session, true);
    const bool ready = Until([&] { return machine->Snapshot().state == workspace::VirtualMachineState::Ready; });
    session.Stop();
    Check(conversation.succeeded && !enteredEarly && preparing && ready && provider->starts == 1 && provider->stops == 1,
        "Guest resource admission ran before voice warmup finished or blocked conversation.");
}

void TestShutdownWhileWaitingForVoiceNeverStartsGuest()
{
    tests::ScopedTestDirectory directory;
    runtime::CompanionPaths paths(directory.root, {"vm-voice-stop", "VM voice stop", "assistant", false});
    OptIn(paths, paths.Descriptor().id);
    auto provider = std::make_shared<Provider>();
    auto machine = std::make_shared<workspace::VirtualMachineRuntime>(provider);
    runtime::ReviaSession session(paths);
    Access::UseVirtualMachine(session, machine);
    Access::SetVoiceWarmupFinished(session, false);
    Access::StartVirtualMachine(session);
    session.Stop();
    Access::SetVoiceWarmupFinished(session, true);
    Check(provider->starts == 0 && provider->stops == 0 && machine->Snapshot().state == workspace::VirtualMachineState::Stopped,
        "Shutdown while waiting for voice entered or stopped an untouched guest provider.");
}

void TestFailedVoiceWarmupDoesNotAdmitGuest()
{
    tests::ScopedTestDirectory directory;
    runtime::CompanionPaths paths(directory.root, {"vm-voice-failed", "VM voice failed", "assistant", false});
    OptIn(paths, paths.Descriptor().id);
    auto provider = std::make_shared<Provider>();
    auto machine = std::make_shared<workspace::VirtualMachineRuntime>(provider);
    runtime::ReviaSession session(paths);
    Access::UseVirtualMachine(session, machine);
    Access::SetVoiceWarmupSucceeded(session, false);
    Access::SetVoiceWarmupFinished(session, true);
    Access::StartVirtualMachine(session);
    const bool blocked = Until([&] { return machine->Snapshot().state == workspace::VirtualMachineState::Blocked; });
    const auto failed = machine->Snapshot();
    session.Stop();
    Check(blocked && failed.code == "startup_prerequisite_failed" && provider->starts == 0 && provider->stops == 0,
        "Failed voice preparation admitted a guest while remote allocations could still be running.");
}

void TestBlockedStartupReportsOnlyAllowlistedReasons()
{
    struct Report
    {
        std::mutex mutex;
        bool received = false;
        bool leaked = false;
        std::string message;
    };
    const std::pair<const char*, const char*> cases[] = {
        {"insufficient_memory", "The Ubuntu workspace needs at least 3 GiB of available host memory. "
                                "Close other applications and relaunch Revia."},
        {"insufficient_disk", "The Ubuntu workspace needs at least 6 GiB of free disk space to start, or 12 GiB for first setup. "
                              "Free space and relaunch Revia."},
        {"missing_tool", "The Ubuntu workspace needs VMware Workstation and Windows OpenSSH client tools. "
                         "Install the missing tools and relaunch Revia."},
        {"readiness_unverified", "The Ubuntu workspace setup could not be verified. Revia will retry on the next launch."},
        {"guest_bootstrap_pending", "The Ubuntu workspace setup could not be verified. Revia will retry on the next launch."},
        {"startup_prerequisite_failed", "The Ubuntu workspace could not finish waiting for Revia's voice preparation. "
                                        "Relaunch Revia to retry."},
        {"enrollment_in_use", "Another Revia session is using this Ubuntu workspace. "
                              "Close that session and relaunch Revia."},
        {"PRIVATE_PROVIDER_UNKNOWN", "The private virtual machine is unavailable; conversation remains available."}
    };
    for (const auto& [code, expected] : cases)
    {
        tests::ScopedTestDirectory directory;
        runtime::CompanionPaths paths(directory.root, {"vm-blocked", "VM blocked", "assistant", false});
        OptIn(paths, paths.Descriptor().id);
        auto provider = std::make_shared<Provider>();
        provider->blockedCode = code;
        runtime::ReviaSession session(paths);
        const auto origin = session.Stamp();
        auto report = std::make_shared<Report>();
        const auto subscription = session.Events().Subscribe([report, origin, code](const runtime::RuntimeEvent& event)
        {
            if (event.component != "Virtual machine") return;
            std::lock_guard lock(report->mutex);
            report->leaked = report->leaked || event.message.find("PRIVATE_PROVIDER") != std::string::npos ||
                             event.message.find(code) != std::string::npos || !event.stamp.SameSession(origin);
            if (event.phase == "Blocked")
            {
                report->message = event.message;
                report->received = true;
            }
        });
        Access::UseVirtualMachine(session, std::make_shared<workspace::VirtualMachineRuntime>(provider));
        Access::StartVirtualMachine(session);
        const bool received = Until([&]
        {
            std::lock_guard lock(report->mutex);
            return report->received;
        });
        session.Stop();
        session.Events().Unsubscribe(subscription);
        std::lock_guard lock(report->mutex);
        Check(received && report->message == expected,
            "Blocked VM startup did not report its fixed actionable explanation.");
        Check(!report->leaked, "Blocked VM startup disclosed provider content, raw codes, or another session identity.");
    }
}
}

void RunVirtualMachineSessionTests()
{
    TestStartupRequiresThisCompanionsOptIn();
    TestStartupKeepsConversationResponsiveAndShutdownCancels();
    TestStatusAndGuestIdentityRemainCompanionLocal();
    TestGuestWaitsForVoiceWithoutBlockingConversation();
    TestShutdownWhileWaitingForVoiceNeverStartsGuest();
    TestFailedVoiceWarmupDoesNotAdmitGuest();
    TestBlockedStartupReportsOnlyAllowlistedReasons();
    std::cout << "VM session startup: 7 tests passed.\n";
}
