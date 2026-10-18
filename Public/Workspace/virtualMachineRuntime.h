#pragma once

#include "Workspace/guestTypes.h"

#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace revia::workspace
{
enum class VirtualMachineState { Disabled, Preparing, Ready, Blocked, Stopping, Stopped };

struct VirtualMachineSnapshot
{
    VirtualMachineState state = VirtualMachineState::Disabled;
    std::string code;
    std::string message;
    GuestIdentity identity;
    bool notificationFailed = false;
    // Operations the guest worker advertised at readiness, and whether its sandbox started an
    // isolated process then. Reported by the guest; the host uses them only to decide what to offer.
    std::vector<std::string> capabilities{};
    bool sandboxReady = false;
};

class IVirtualMachineProvider
{
  public:
    virtual ~IVirtualMachineProvider() = default;
    [[nodiscard]] virtual VirtualMachineSnapshot EnsureReady(std::stop_token stop) = 0;
    [[nodiscard]] virtual GuestResult Execute(const GuestRequest& request, const GuestLimits& limits, std::stop_token stop) = 0;
    [[nodiscard]] virtual GuestResult StopGuest() = 0;
};

// Owns this companion's VM lifecycle. Providers own transport, never identity or conversation.
class VirtualMachineRuntime final
{
  public:
    using Observer = std::function<void(const VirtualMachineSnapshot&)>;
    using StartupPrerequisite = std::function<bool(std::stop_token)>;
    explicit VirtualMachineRuntime(std::shared_ptr<IVirtualMachineProvider> provider);
    ~VirtualMachineRuntime();
    VirtualMachineRuntime(const VirtualMachineRuntime&) = delete;
    VirtualMachineRuntime& operator=(const VirtualMachineRuntime&) = delete;

    // The observer may enqueue status; it must not call lifecycle methods or block.
    void SetObserver(Observer observer);
    // Runs the optional host prerequisite on the owned startup worker. It must be
    // bounded and observe cancellation; failure never enters the provider.
    void Start(StartupPrerequisite prerequisite = {});
    void Stop();
    [[nodiscard]] VirtualMachineSnapshot Snapshot() const;
    [[nodiscard]] GuestResult Execute(const GuestRequest& request, const GuestLimits& limits, std::stop_token stop = {});

  private:
    void Publish(VirtualMachineSnapshot snapshot, std::stop_token preparation = {});
    std::shared_ptr<IVirtualMachineProvider> provider;
    mutable std::mutex stateMutex;
    std::mutex lifecycleMutex;
    std::mutex operationMutex;
    VirtualMachineSnapshot snapshot;
    Observer observer;
    std::stop_source operationStop;
    std::jthread startup;
    bool started = false;
    // Guarded by operationMutex; retained across retries until shutdown is confirmed.
    bool providerEntered = false;
};

[[nodiscard]] const char* ToString(VirtualMachineState state);
}
