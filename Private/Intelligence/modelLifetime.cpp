#include "Intelligence/modelLifetime.h"

#include <algorithm>
#include <utility>

namespace revia::intelligence
{

namespace
{

std::uint64_t ElapsedMs(const std::chrono::steady_clock::time_point& since)
{
    if (since == std::chrono::steady_clock::time_point{}) return 0;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - since).count());
}

} // namespace

ModelLifetimeCoordinator::Lease::Lease(
    ModelLifetimeCoordinator* owner, const IntelligenceTier tier)
    : coordinator(owner), held(tier)
{
}

ModelLifetimeCoordinator::Lease::~Lease()
{
    Release();
}

ModelLifetimeCoordinator::Lease::Lease(Lease&& other) noexcept
    : coordinator(other.coordinator), held(other.held)
{
    other.coordinator = nullptr;
}

ModelLifetimeCoordinator::Lease& ModelLifetimeCoordinator::Lease::operator=(
    Lease&& other) noexcept
{
    if (this != &other)
    {
        Release();
        coordinator = other.coordinator;
        held = other.held;
        other.coordinator = nullptr;
    }
    return *this;
}

void ModelLifetimeCoordinator::Lease::Release()
{
    if (coordinator == nullptr) return;
    // Cleared before the call, so a release that throws cannot be repeated by the
    // destructor running afterwards.
    ModelLifetimeCoordinator* owner = coordinator;
    coordinator = nullptr;
    owner->ReleaseLease(held);
}

ModelLifetimeCoordinator::ModelLifetimeCoordinator(ModelResidencyManager& inventory)
    : residency(inventory)
{
}

ModelLifetimeCoordinator::Managed* ModelLifetimeCoordinator::FindUnlocked(
    const IntelligenceTier tier)
{
    const auto found = std::find_if(managed.begin(), managed.end(),
        [tier](const Managed& entry) { return entry.tier == tier; });
    return found == managed.end() ? nullptr : &*found;
}

const ModelLifetimeCoordinator::Managed* ModelLifetimeCoordinator::FindUnlocked(
    const IntelligenceTier tier) const
{
    const auto found = std::find_if(managed.begin(), managed.end(),
        [tier](const Managed& entry) { return entry.tier == tier; });
    return found == managed.end() ? nullptr : &*found;
}

void ModelLifetimeCoordinator::Manage(
    const IntelligenceTier tier,
    Activator activate,
    Deactivator deactivate,
    const ModelLifetimePolicy policy)
{
    std::lock_guard lock(mutex);
    Managed* entry = FindUnlocked(tier);
    if (entry == nullptr)
    {
        managed.push_back(Managed{});
        entry = &managed.back();
        entry->tier = tier;
    }
    entry->activate = std::move(activate);
    entry->deactivate = std::move(deactivate);
    entry->policy = policy;
}

bool ModelLifetimeCoordinator::IsManaged(const IntelligenceTier tier) const
{
    std::lock_guard lock(mutex);
    const Managed* entry = FindUnlocked(tier);
    return entry != nullptr && static_cast<bool>(entry->activate);
}

ModelLifetimePolicy ModelLifetimeCoordinator::Policy(const IntelligenceTier tier) const
{
    std::lock_guard lock(mutex);
    const Managed* entry = FindUnlocked(tier);
    return entry == nullptr ? ModelLifetimePolicy{} : entry->policy;
}

std::uint32_t ModelLifetimeCoordinator::ActiveLeases(const IntelligenceTier tier) const
{
    std::lock_guard lock(mutex);
    const Managed* entry = FindUnlocked(tier);
    return entry == nullptr ? 0U : entry->leases;
}

ModelLifetimeCoordinator::Lease ModelLifetimeCoordinator::Acquire(
    const IntelligenceTier tier, std::stop_token stopToken)
{
    Activator activate;
    {
        std::unique_lock lock(mutex);
        Managed* entry = FindUnlocked(tier);
        if (entry == nullptr || !entry->activate) return {};

        // Someone else is already bringing this up, or putting it away. Wait for that
        // to finish rather than racing it: two llama.cpp processes for one role fit in
        // memory exactly once, and a Start() overlapping a Stop() on the same process
        // object is worse than either.
        while ((entry->loading || entry->unloading) && !stopToken.stop_requested())
        {
            loadFinished.wait_for(lock, std::chrono::milliseconds(100));
            entry = FindUnlocked(tier);
            if (entry == nullptr) return {};
        }
        if (stopToken.stop_requested()) return {};

        // Counted before the load, so a sweep running concurrently sees the role as
        // spoken for and cannot evict what is about to be used.
        ++entry->leases;
        if (entry->resident)
        {
            return Lease(this, tier);
        }
        entry->loading = true;
        activate = entry->activate;
    }

    // Outside the lock on purpose. A load takes seconds and blocks on a child process;
    // holding the coordinator's mutex across it would stall every other role's
    // bookkeeping and every status query behind it.
    residency.MarkLoading(tier);
    bool ready = false;
    try
    {
        ready = activate(stopToken);
    }
    catch (...)
    {
        std::lock_guard lock(mutex);
        if (Managed* entry = FindUnlocked(tier))
        {
            entry->loading = false;
            if (entry->leases > 0) --entry->leases;
        }
        loadFinished.notify_all();
        throw;
    }

    {
        std::lock_guard lock(mutex);
        Managed* entry = FindUnlocked(tier);
        if (entry != nullptr)
        {
            entry->loading = false;
            entry->resident = ready;
            if (ready) entry->residentSince = std::chrono::steady_clock::now();
            else if (entry->leases > 0) --entry->leases;
        }
    }
    loadFinished.notify_all();

    // A load that did not come up is not a lease. The caller gets an empty one and
    // takes its own fallback, which is the deliberate choice the assignment asks for
    // rather than a silent wait.
    return ready ? Lease(this, tier) : Lease{};
}

void ModelLifetimeCoordinator::ReleaseLease(const IntelligenceTier tier)
{
    std::lock_guard lock(mutex);
    if (Managed* entry = FindUnlocked(tier); entry != nullptr && entry->leases > 0)
    {
        --entry->leases;
        if (entry->leases == 0) entry->lastReleased = std::chrono::steady_clock::now();
    }
}

void ModelLifetimeCoordinator::SweepIdle()
{
    // Collected under the lock and called outside it, for the same reason a load is:
    // stopping a child process is slow and must not hold up status queries.
    std::vector<std::pair<IntelligenceTier, Deactivator>> release;
    {
        std::lock_guard lock(mutex);
        for (Managed& entry : managed)
        {
            if (!entry.policy.onDemand || !entry.deactivate) continue;
            if (!entry.resident || entry.loading || entry.unloading) continue;
            if (entry.leases > 0) continue;
            // A model mid-answer is in use even if no lease is held, which can happen
            // while a caller that predates this coordinator is running. Never evict
            // something that is generating.
            if (residency.IsInferenceActive(entry.tier)) continue;
            if (ElapsedMs(entry.residentSince) < entry.policy.minimumResidencyMs) continue;
            if (ElapsedMs(entry.lastReleased) < entry.policy.idleGraceMs) continue;

            entry.resident = false;
            entry.unloading = true;
            release.emplace_back(entry.tier, entry.deactivate);
        }
    }

    for (const auto& [tier, deactivate] : release)
    {
        // The flag is cleared on every path out, including a throwing deactivator: a
        // role left marked unloading would never be loadable again.
        try
        {
            deactivate();
        }
        catch (...)
        {
            FinishUnload(tier);
            throw;
        }
        // Unloaded, not Failed. Nothing went wrong here, and a role reported as failed
        // is one routing stops trying to bring back.
        residency.MarkUnloaded(tier, "Put away after being idle.");
        FinishUnload(tier);
    }
}

bool ModelLifetimeCoordinator::ReleaseNow(const IntelligenceTier tier)
{
    Deactivator deactivate;
    {
        std::lock_guard lock(mutex);
        Managed* entry = FindUnlocked(tier);
        if (entry == nullptr || !entry->deactivate) return false;
        if (!entry->resident || entry->loading || entry->unloading) return false;
        if (entry->leases > 0) return false;
        if (residency.IsInferenceActive(tier)) return false;
        entry->resident = false;
        entry->unloading = true;
        deactivate = entry->deactivate;
    }
    try
    {
        deactivate();
    }
    catch (...)
    {
        FinishUnload(tier);
        throw;
    }
    residency.MarkUnloaded(tier, "Put away on request.");
    FinishUnload(tier);
    return true;
}

void ModelLifetimeCoordinator::FinishUnload(const IntelligenceTier tier)
{
    {
        std::lock_guard lock(mutex);
        if (Managed* entry = FindUnlocked(tier)) entry->unloading = false;
    }
    // Woken for the same reason a finished load is: a request waiting on this role can
    // now go ahead and bring it back.
    loadFinished.notify_all();
}

} // namespace revia::intelligence
