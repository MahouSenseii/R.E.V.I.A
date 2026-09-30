#pragma once

#include "Intelligence/intelligenceTypes.h"
#include "Intelligence/modelResidencyManager.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::intelligence
{

// Idle eviction grace and minimum residency prevent repeated unload/reload churn.
struct ModelLifetimePolicy
{
    // Off by default, everywhere. On-demand residency changes when a model is present,
    // which changes latency and memory at once, and that is not a default anybody
    // should acquire by upgrading.
    bool onDemand = false;
    std::uint64_t idleGraceMs = 300000;
    std::uint64_t minimumResidencyMs = 60000;
};

// Decides model residency through session-owned load/unload callbacks; owns no process.
// SweepIdle runs on an existing tick; this class starts no scheduler.
class ModelLifetimeCoordinator
{
public:
    // Brings the role up if it is not up. Returns whether it is usable now.
    using Activator = std::function<bool(std::stop_token)>;
    // Puts it away. Called only when nothing holds a lease and the policy says so.
    using Deactivator = std::function<void()>;

    // A role is in use for as long as one of these exists.
    //
    // Exception-safe by construction: the count comes back down when the lease is
    // destroyed, on any path out of the scope that holds it. A lease that failed to
    // acquire is falsy and holds nothing, so a caller that forgets to check releases
    // nothing rather than releasing someone else's.
    class Lease
    {
    public:
        Lease() = default;
        Lease(ModelLifetimeCoordinator* owner, IntelligenceTier tier);
        ~Lease();

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;

        [[nodiscard]] explicit operator bool() const { return coordinator != nullptr; }
        void Release();

    private:
        ModelLifetimeCoordinator* coordinator = nullptr;
        IntelligenceTier held = IntelligenceTier::Main;
    };

    explicit ModelLifetimeCoordinator(ModelResidencyManager& inventory);

    ModelLifetimeCoordinator(const ModelLifetimeCoordinator&) = delete;
    ModelLifetimeCoordinator& operator=(const ModelLifetimeCoordinator&) = delete;

    // Installed once by the session that owns the process. A role with no activator is
    // not managed here and is never swept.
    void Manage(IntelligenceTier tier, Activator activate, Deactivator deactivate, ModelLifetimePolicy policy);

    [[nodiscard]] bool IsManaged(IntelligenceTier tier) const;
    [[nodiscard]] ModelLifetimePolicy Policy(IntelligenceTier tier) const;

    // Returns a role lease; concurrent acquisitions share one load.
    // Cancellation while waiting returns an empty lease, which callers must not use to proceed.
    [[nodiscard]] Lease Acquire(IntelligenceTier tier, std::stop_token stopToken = {});

    // Puts away every managed role that is resident, unused, past its idle grace and
    // past its minimum residency. Safe to call as often as anything already ticks.
    void SweepIdle();

    // Puts away one role now if nothing holds it, regardless of grace. For shutdown and
    // for an explicit request; never called by the sweep.
    bool ReleaseNow(IntelligenceTier tier);

    [[nodiscard]] std::uint32_t ActiveLeases(IntelligenceTier tier) const;

private:
    // Lease is a member class and already has access to everything below.
    struct Managed
    {
        IntelligenceTier tier = IntelligenceTier::Main;
        Activator activate;
        Deactivator deactivate;
        ModelLifetimePolicy policy;
        std::uint32_t leases = 0;
        bool loading = false;
        // Set while the deactivator is running, which happens outside the lock because
        // stopping a child process is slow. Without it a request arriving mid-unload
        // would see the role as not resident, start a load, and run Start() against the
        // same process object that Stop() is still working on.
        bool unloading = false;
        bool resident = false;
        std::chrono::steady_clock::time_point residentSince{};
        std::chrono::steady_clock::time_point lastReleased{};
    };

    Managed* FindUnlocked(IntelligenceTier tier);
    const Managed* FindUnlocked(IntelligenceTier tier) const;
    void ReleaseLease(IntelligenceTier tier);
    void FinishUnload(IntelligenceTier tier);

    ModelResidencyManager& residency;
    mutable std::mutex mutex;
    // Woken when a load finishes, so a second caller for the same role waits for the
    // first rather than starting its own.
    std::condition_variable loadFinished;
    std::vector<Managed> managed;
};

} // namespace revia::intelligence
