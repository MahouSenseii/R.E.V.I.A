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

// How long a role may sit unused before it is put away, and how long it is safe from
// being put away after it arrives.
//
// The minimum exists because a load is expensive and an idle sweep is cheap. Without
// it, a role loaded for one question and then left alone for a moment gets evicted
// immediately and reloaded by the next question, which costs more than never unloading
// at all. Together the two turn "unload when idle" into "unload when actually idle".
struct ModelLifetimePolicy
{
    // Off by default, everywhere. On-demand residency changes when a model is present,
    // which changes latency and memory at once, and that is not a default anybody
    // should acquire by upgrading.
    bool onDemand = false;
    std::uint64_t idleGraceMs = 300000;
    std::uint64_t minimumResidencyMs = 60000;
};

// Owns when a model role is resident, and nothing else.
//
// It does not own the process. ReviaSession does, and this is handed two callables that
// reach into that ownership: one that makes the role available and one that puts it
// away. That is the whole reason it is not a second manager -- it decides *when*, and
// the existing owner still decides *how*, keeps the handle, and remains the only thing
// that can terminate a server it started.
//
// It is also not a scheduler. Nothing here runs on its own; `SweepIdle` is called by
// whatever already ticks, and a call that finds nothing to do returns immediately.
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
    void Manage(
        IntelligenceTier tier,
        Activator activate,
        Deactivator deactivate,
        ModelLifetimePolicy policy);

    [[nodiscard]] bool IsManaged(IntelligenceTier tier) const;
    [[nodiscard]] ModelLifetimePolicy Policy(IntelligenceTier tier) const;

    // Acquire the role for the duration of the returned lease.
    //
    // Concurrent calls for a role that is loading wait for that one load rather than
    // starting a second: a duplicate llama.cpp process would fit in memory exactly once
    // and the second would fail in a way the first would be blamed for. A caller whose
    // token is stopped while waiting gets an empty lease and loads nothing.
    //
    // An empty lease is a real answer and never a reason to proceed anyway. Cold and
    // unavailable are different, and this is where the difference is decided: it tries,
    // and only a failure to bring the role up is unavailability.
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
