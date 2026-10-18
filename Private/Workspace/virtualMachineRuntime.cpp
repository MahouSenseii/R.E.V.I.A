#include "Workspace/virtualMachineRuntime.h"

#include <utility>

namespace revia::workspace
{
namespace
{
bool SameGuest(const GuestIdentity& left, const GuestIdentity& right)
{
    return !left.environmentId.empty() && !left.bootId.empty() &&
        left.environmentId == right.environmentId && left.bootId == right.bootId;
}

GuestResult Refused(const char* code, const char* message)
{
    GuestResult result;
    result.code = code;
    result.message = message;
    return result;
}
}

VirtualMachineRuntime::VirtualMachineRuntime(std::shared_ptr<IVirtualMachineProvider> input) : provider(std::move(input)) {}

VirtualMachineRuntime::~VirtualMachineRuntime()
{
    try { Stop(); }
    catch (...) { operationStop.request_stop(); startup.request_stop(); }
}

void VirtualMachineRuntime::SetObserver(Observer input)
{
    std::lock_guard lock(stateMutex);
    observer = std::move(input);
}

VirtualMachineSnapshot VirtualMachineRuntime::Snapshot() const
{
    std::lock_guard lock(stateMutex);
    return snapshot;
}

void VirtualMachineRuntime::Publish(VirtualMachineSnapshot value, std::stop_token preparation)
{
    Observer notify;
    {
        std::lock_guard lock(stateMutex);
        if (preparation.stop_possible() &&
            (preparation.stop_requested() || snapshot.state != VirtualMachineState::Preparing)) return;
        snapshot = value;
        notify = observer;
    }
    if (!notify) return;
    try { notify(value); }
    catch (...)
    {
        std::lock_guard lock(stateMutex);
        snapshot.notificationFailed = true;
    }
}

void VirtualMachineRuntime::Start(StartupPrerequisite prerequisite)
{
    std::lock_guard lifecycle(lifecycleMutex);
    const auto current = Snapshot().state;
    if (current == VirtualMachineState::Preparing || current == VirtualMachineState::Ready) return;
    if (!provider)
    {
        Publish({VirtualMachineState::Blocked, "provider_unavailable", "The virtual machine provider is unavailable.", {}, false});
        return;
    }
    if (startup.joinable()) startup.join();
    {
        std::lock_guard lock(stateMutex);
        operationStop = std::stop_source{};
    }
    started = true;
    Publish({VirtualMachineState::Preparing, "preparing", "Preparing the private virtual machine.", {}, false});
    try
    {
        startup = std::jthread([this, prerequisite = std::move(prerequisite)](std::stop_token stop)
        {
            std::lock_guard operation(operationMutex);
            VirtualMachineSnapshot result;
            if (stop.stop_requested()) return;
            if (prerequisite)
            {
                bool admitted = false;
                try { admitted = prerequisite(stop); }
                catch (...) {}
                if (stop.stop_requested()) return;
                if (!admitted)
                {
                    Publish({VirtualMachineState::Blocked, "startup_prerequisite_failed",
                        "The host startup prerequisite did not finish.", {}, false}, stop);
                    return;
                }
            }
            if (stop.stop_requested()) return;
            providerEntered = true;
            try { result = provider->EnsureReady(stop); }
            catch (...)
            {
                result = {VirtualMachineState::Blocked, "provisioning_failed", "The virtual machine provider failed during setup.", {}, false};
            }
            if (stop.stop_requested()) return;
            if (result.state != VirtualMachineState::Ready || !SameGuest(result.identity, result.identity))
            {
                result.state = VirtualMachineState::Blocked;
                if (result.code.empty()) result.code = "readiness_unverified";
                if (result.message.empty()) result.message = "The virtual machine has not proved readiness.";
            }
            Publish(std::move(result), stop);
        });
    }
    catch (...)
    {
        Publish({VirtualMachineState::Blocked, "startup_failed", "The virtual machine startup worker could not start.", {}, false});
    }
}

void VirtualMachineRuntime::Stop()
{
    std::lock_guard lifecycle(lifecycleMutex);
    if (!started) return;
    started = false;
    std::stop_source cancellation;
    {
        std::lock_guard lock(stateMutex);
        cancellation = operationStop;
    }
    // request_stop synchronously invokes provider callbacks; they may inspect status.
    cancellation.request_stop();
    startup.request_stop();
    Publish({VirtualMachineState::Stopping, "stopping", "Stopping the private virtual machine.", {}, false});
    if (startup.joinable()) startup.join();
    std::lock_guard operation(operationMutex);
    if (!providerEntered)
    {
        Publish({VirtualMachineState::Stopped, "stopped", "Virtual machine preparation stopped before provisioning.", {}, false});
        return;
    }
    GuestResult result;
    try { result = provider->StopGuest(); }
    catch (...) { result.code = "shutdown_failed"; }
    if (result.succeeded)
    {
        providerEntered = false;
        Publish({VirtualMachineState::Stopped, "stopped", "The private virtual machine stopped.", {}, false});
    }
    else
        Publish({VirtualMachineState::Blocked, "shutdown_unverified", "The private virtual machine did not confirm shutdown.", {}, false});
}

GuestResult VirtualMachineRuntime::Execute(const GuestRequest& request, const GuestLimits& limits, std::stop_token stop)
{
    // Provisioning can take minutes. Refuse immediately instead of waiting behind it.
    if (Snapshot().state != VirtualMachineState::Ready)
        return Refused("guest_not_ready", "The private virtual machine is not ready.");
    std::lock_guard operation(operationMutex);
    std::stop_token shutdown;
    {
        std::lock_guard lock(stateMutex);
        if (snapshot.state != VirtualMachineState::Ready)
            return Refused("guest_not_ready", "The private virtual machine is not ready.");
        if (!SameGuest(request.expectedIdentity, snapshot.identity))
            return Refused("guest_identity_mismatch", "The request does not belong to the admitted virtual machine boot.");
        shutdown = operationStop.get_token();
    }
    std::stop_source cancellation;
    std::stop_callback requested(stop, [&] { cancellation.request_stop(); });
    std::stop_callback shuttingDown(shutdown, [&] { cancellation.request_stop(); });
    if (cancellation.stop_requested())
    {
        auto result = Refused("cancelled", "The guest request was cancelled before execution.");
        result.cancelled = true;
        return result;
    }
    const bool effect = request.operation == GuestOperation::WriteText || request.operation == GuestOperation::ExecuteProcess ||
        request.operation == GuestOperation::RunPython || request.operation == GuestOperation::ImportFiles;
    GuestResult result;
    try { result = provider->Execute(request, limits, cancellation.get_token()); }
    catch (...)
    {
        result = Refused("guest_request_failed", "The guest provider failed after the request was submitted.");
        result.attempted = true;
        result.uncertainEffect = effect;
        Publish({VirtualMachineState::Blocked, "guest_request_failed", "The guest connection must be checked before further work.", {}, false});
        return result;
    }
    if (result.succeeded && !result.receiptVerified)
    {
        result.succeeded = false;
        result.uncertainEffect = effect && result.attempted;
        result.code = "guest_receipt_unverified";
        result.message = "The guest provider did not verify its success receipt.";
        Publish({VirtualMachineState::Blocked, "guest_receipt_unverified", "The guest receipt must be verified before further work.", {}, false});
        return result;
    }
    if ((result.attempted || result.succeeded || !result.identity.environmentId.empty()) &&
        !SameGuest(request.expectedIdentity, result.identity))
    {
        result.succeeded = false;
        result.uncertainEffect = result.uncertainEffect || (effect && result.attempted);
        result.code = "guest_receipt_mismatch";
        result.message = "The guest receipt does not belong to the admitted virtual machine boot.";
        Publish({VirtualMachineState::Blocked, "guest_receipt_mismatch", "The guest boot must be checked before further work.", {}, false});
    }
    else if (result.uncertainEffect && !result.receiptVerified)
        Publish({VirtualMachineState::Blocked, "guest_effect_unverified", "A guest effect was not confirmed; dependent work has stopped.", {}, false});
    return result;
}

const char* ToString(VirtualMachineState state)
{
    switch (state)
    {
    case VirtualMachineState::Disabled: return "Disabled";
    case VirtualMachineState::Preparing: return "Preparing";
    case VirtualMachineState::Ready: return "Ready";
    case VirtualMachineState::Blocked: return "Blocked";
    case VirtualMachineState::Stopping: return "Stopping";
    case VirtualMachineState::Stopped: return "Stopped";
    }
    return "Blocked";
}
}
