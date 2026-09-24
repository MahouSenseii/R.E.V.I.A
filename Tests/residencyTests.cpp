#include "testSupport.h"

#include "Intelligence/modelLifetime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace revia::intelligence;
using revia::tests::Check;

// When a model is resident, exercised without a model.
//
// The coordinator decides *when*, and is handed two callables that stand in for the
// session's own process ownership. That separation is what lets the whole policy --
// leases, idle grace, minimum residency, coalesced loads, no eviction while in use --
// be tested with no llama.cpp, no GPU and no waiting.

struct Fixture
{
    ModelResidencyManager inventory;
    ModelLifetimeCoordinator coordinator{inventory};
    std::atomic<int> loads{0};
    std::atomic<int> unloads{0};
    std::atomic<bool> loadSucceeds{true};
    std::atomic<int> loadDelayMs{0};

    explicit Fixture(const ModelLifetimePolicy policy = {})
    {
        ModelResidency expert;
        expert.tier = IntelligenceTier::Expert;
        expert.role = "Expert";
        expert.state = ResidencyState::Unloaded;
        inventory.Register(std::move(expert));

        coordinator.Manage(
            IntelligenceTier::Expert,
            [this](std::stop_token token)
            {
                ++loads;
                const int delay = loadDelayMs.load();
                if (delay > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                if (token.stop_requested()) return false;
                if (!loadSucceeds.load()) return false;
                inventory.MarkReady(IntelligenceTier::Expert, 1.0, true);
                return true;
            },
            [this]() { ++unloads; },
            policy);
    }
};

ModelLifetimePolicy Eager()
{
    // Zero grace and zero minimum: the sweep is being tested, not the clock.
    ModelLifetimePolicy policy;
    policy.onDemand = true;
    policy.idleGraceMs = 0;
    policy.minimumResidencyMs = 0;
    return policy;
}

// A tier nobody manages is never touched. This is the default everywhere, and it is the
// shape of "nothing changed unless it was asked for".
void TestAnUnmanagedTierIsNeverPutAway()
{
    ModelResidencyManager inventory;
    ModelLifetimeCoordinator coordinator{inventory};
    Check(!coordinator.IsManaged(IntelligenceTier::Expert),
        "A tier was managed without anyone asking for it.");
    auto lease = coordinator.Acquire(IntelligenceTier::Expert);
    Check(!lease, "An unmanaged tier handed out a lease.");
    coordinator.SweepIdle();  // must not crash, must do nothing
}

// On-demand off is the default, and a managed tier with it off is never swept even when
// it has been idle forever.
void TestOnDemandOffLeavesAModelAlone()
{
    Fixture fixture;  // default policy: onDemand false
    {
        auto lease = fixture.coordinator.Acquire(IntelligenceTier::Expert);
        Check(static_cast<bool>(lease), "A managed tier refused to load on request.");
    }
    fixture.coordinator.SweepIdle();
    fixture.coordinator.SweepIdle();
    Check(fixture.unloads.load() == 0,
        "A model was put away while on-demand residency was switched off.");
}

// The lease is what stops an idle sweep from evicting a model mid-answer.
void TestAModelInUseIsNotEvicted()
{
    Fixture fixture{Eager()};
    auto lease = fixture.coordinator.Acquire(IntelligenceTier::Expert);
    Check(static_cast<bool>(lease) && fixture.loads.load() == 1,
        "The first request did not load the model.");
    Check(fixture.coordinator.ActiveLeases(IntelligenceTier::Expert) == 1,
        "A held lease was not counted.");

    fixture.coordinator.SweepIdle();
    Check(fixture.unloads.load() == 0, "A model was evicted while it was in use.");

    // A second holder keeps it alive after the first lets go.
    {
        auto second = fixture.coordinator.Acquire(IntelligenceTier::Expert);
        Check(static_cast<bool>(second) && fixture.loads.load() == 1,
            "An already resident model was loaded a second time.");
        Check(fixture.coordinator.ActiveLeases(IntelligenceTier::Expert) == 2,
            "Concurrent use was not counted.");
        lease.Release();
        fixture.coordinator.SweepIdle();
        Check(fixture.unloads.load() == 0,
            "A model was evicted while another holder still had it.");
    }

    // Both gone: now it may be put away, and the record says it was put away rather
    // than that it failed.
    fixture.coordinator.SweepIdle();
    Check(fixture.unloads.load() == 1, "An idle, unused model was never put away.");
    const auto snapshot = fixture.inventory.Snapshot();
    Check(!snapshot.empty() && snapshot.front().state == ResidencyState::Unloaded,
        "A model put away on purpose was not reported as unloaded.");
    Check(!fixture.inventory.IsResident(IntelligenceTier::Expert),
        "An unloaded model still counted as resident.");
}

// Being put away is not failing, and the difference decides whether anything tries
// again. A model that was evicted must come back on the next request.
void TestAnUnloadedModelComesBack()
{
    Fixture fixture{Eager()};
    {
        auto lease = fixture.coordinator.Acquire(IntelligenceTier::Expert);
        Check(static_cast<bool>(lease), "The first load failed.");
    }
    fixture.coordinator.SweepIdle();
    Check(fixture.unloads.load() == 1, "The model was not put away.");

    auto again = fixture.coordinator.Acquire(IntelligenceTier::Expert);
    Check(static_cast<bool>(again) && fixture.loads.load() == 2,
        "A model that had been put away was treated as gone for good.");
}

// A load that does not come up is unavailability, and the caller is told so rather than
// left holding something that is not there.
void TestAFailedLoadIsNotALease()
{
    Fixture fixture{Eager()};
    fixture.loadSucceeds.store(false);
    auto lease = fixture.coordinator.Acquire(IntelligenceTier::Expert);
    Check(!lease, "A failed load still handed out a lease.");
    Check(fixture.coordinator.ActiveLeases(IntelligenceTier::Expert) == 0,
        "A failed load left a lease behind.");
    // And it did not mark itself resident, so a sweep has nothing to release.
    fixture.coordinator.SweepIdle();
    Check(fixture.unloads.load() == 0, "A model that never loaded was unloaded anyway.");
}

// Two callers arriving at once share one load. A second llama.cpp for the same role
// fits in memory exactly once, and the loser of that race would fail in a way the
// winner gets blamed for.
void TestConcurrentRequestsShareOneLoad()
{
    Fixture fixture{Eager()};
    fixture.loadDelayMs.store(120);

    std::atomic<int> granted{0};
    std::vector<std::thread> callers;
    callers.reserve(4);
    for (int index = 0; index < 4; ++index)
    {
        callers.emplace_back([&fixture, &granted]()
        {
            auto lease = fixture.coordinator.Acquire(IntelligenceTier::Expert);
            if (lease) ++granted;
            // Held briefly so the others overlap with this one.
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        });
    }
    for (std::thread& caller : callers) caller.join();

    Check(granted.load() == 4, "A concurrent request was refused a resident model.");
    Check(fixture.loads.load() == 1,
        "Concurrent requests started more than one load of the same model.");
    Check(fixture.coordinator.ActiveLeases(IntelligenceTier::Expert) == 0,
        "Leases were not released when their holders finished.");
}

// Cancellation during a wait produces no lease and loads nothing.
void TestACancelledRequestLoadsNothing()
{
    Fixture fixture{Eager()};
    fixture.loadDelayMs.store(150);

    std::stop_source stop;
    std::thread holder([&fixture]()
    {
        auto lease = fixture.coordinator.Acquire(IntelligenceTier::Expert);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    });
    // Let the first caller take the loading flag, then ask with a token already stopped.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    stop.request_stop();
    auto refused = fixture.coordinator.Acquire(IntelligenceTier::Expert, stop.get_token());
    Check(!refused, "A cancelled request was handed a lease.");
    holder.join();
    Check(fixture.loads.load() == 1,
        "A cancelled request started a load of its own.");
}

// The minimum residency is what stops load/evict/load from costing more than never
// unloading at all.
void TestAFreshlyLoadedModelIsNotImmediatelyEvicted()
{
    ModelLifetimePolicy policy;
    policy.onDemand = true;
    policy.idleGraceMs = 0;
    policy.minimumResidencyMs = 60000;  // a minute; this test takes milliseconds
    Fixture fixture{policy};

    {
        auto lease = fixture.coordinator.Acquire(IntelligenceTier::Expert);
        Check(static_cast<bool>(lease), "The model did not load.");
    }
    fixture.coordinator.SweepIdle();
    Check(fixture.unloads.load() == 0,
        "A model was evicted seconds after being loaded for something.");
}

// An explicit release ignores the grace but still refuses to take a model away from
// something using it.
void TestAnExplicitReleaseStillRespectsUse()
{
    ModelLifetimePolicy policy;
    policy.onDemand = true;
    policy.idleGraceMs = 600000;
    policy.minimumResidencyMs = 600000;
    Fixture fixture{policy};

    auto lease = fixture.coordinator.Acquire(IntelligenceTier::Expert);
    Check(static_cast<bool>(lease), "The model did not load.");
    Check(!fixture.coordinator.ReleaseNow(IntelligenceTier::Expert),
        "An explicit release took a model away from something using it.");
    lease.Release();
    Check(fixture.coordinator.ReleaseNow(IntelligenceTier::Expert),
        "An explicit release did not put away an unused model.");
    Check(fixture.unloads.load() == 1, "The explicit release did not reach the owner.");
}

// A model generating an answer is in use even when no lease is held -- which is the
// case for any caller that predates the coordinator.
void TestAGeneratingModelIsNotEvicted()
{
    Fixture fixture{Eager()};
    {
        auto lease = fixture.coordinator.Acquire(IntelligenceTier::Expert);
        Check(static_cast<bool>(lease), "The model did not load.");
    }
    fixture.inventory.BeginInference(IntelligenceTier::Expert, "interactive");
    fixture.coordinator.SweepIdle();
    Check(fixture.unloads.load() == 0,
        "A model was put away while it was generating an answer.");
    fixture.inventory.EndInference(IntelligenceTier::Expert);
    fixture.coordinator.SweepIdle();
    Check(fixture.unloads.load() == 1,
        "A model that finished generating was never put away.");
}

// A request arriving while the model is being put away waits for that to finish.
//
// Stopping a child process is slow, so the deactivator runs outside the coordinator's
// lock. Without a flag saying so, a request arriving in that window would see the role
// as not resident and call Start() on the same process object Stop() is still working
// on.
void TestARequestDuringAnUnloadWaitsForIt()
{
    Fixture fixture{Eager()};
    std::atomic<bool> unloadRunning{false};
    std::atomic<bool> overlapped{false};

    // Re-managed with a slow deactivator and an activator that reports any overlap.
    ModelLifetimePolicy policy = Eager();
    fixture.coordinator.Manage(
        IntelligenceTier::Expert,
        [&fixture, &unloadRunning, &overlapped](std::stop_token)
        {
            if (unloadRunning.load()) overlapped.store(true);
            ++fixture.loads;
            fixture.inventory.MarkReady(IntelligenceTier::Expert, 1.0, true);
            return true;
        },
        [&fixture, &unloadRunning]()
        {
            unloadRunning.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            ++fixture.unloads;
            unloadRunning.store(false);
        },
        policy);

    {
        auto lease = fixture.coordinator.Acquire(IntelligenceTier::Expert);
        Check(static_cast<bool>(lease), "The model did not load.");
    }

    std::thread sweeper([&fixture]() { fixture.coordinator.SweepIdle(); });
    // Long enough for the sweep to be inside the slow deactivator.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    auto during = fixture.coordinator.Acquire(IntelligenceTier::Expert);
    sweeper.join();

    Check(static_cast<bool>(during),
        "A request that arrived during an unload was refused instead of waiting.");
    Check(!overlapped.load(),
        "A load was started while the same model was still being put away.");
    Check(fixture.unloads.load() == 1, "The unload did not complete.");
}

// Failure and intentional unloading must stay distinguishable in the record.
void TestUnloadedAndFailedStayDistinct()
{
    ModelResidencyManager inventory;
    ModelResidency expert;
    expert.tier = IntelligenceTier::Expert;
    expert.role = "Expert";
    inventory.Register(std::move(expert));

    inventory.MarkFailed(IntelligenceTier::Expert, "It would not start.");
    Check(inventory.Snapshot().front().state == ResidencyState::Failed,
        "A failed model was not recorded as failed.");
    inventory.MarkUnloaded(IntelligenceTier::Expert, "Put away.");
    Check(inventory.Snapshot().front().state == ResidencyState::Unloaded &&
        ToString(ResidencyState::Unloaded) == "Unloaded",
        "A model put away on purpose was recorded as something else.");

    // A role with no artifacts is not made loadable by being swept.
    ModelResidency absent;
    absent.tier = IntelligenceTier::Fast;
    absent.role = "Fast";
    absent.state = ResidencyState::Disabled;
    inventory.Register(std::move(absent));
    inventory.MarkUnloaded(IntelligenceTier::Fast, "Put away.");
    const auto snapshot = inventory.Snapshot();
    const auto fast = std::find_if(snapshot.begin(), snapshot.end(),
        [](const ModelResidency& model) { return model.tier == IntelligenceTier::Fast; });
    Check(fast != snapshot.end() && fast->state == ResidencyState::Disabled,
        "A disabled role was promoted to loadable by an idle sweep.");
}

} // namespace

void RunResidencyTests()
{
    TestAnUnmanagedTierIsNeverPutAway();
    TestOnDemandOffLeavesAModelAlone();
    TestAModelInUseIsNotEvicted();
    TestAnUnloadedModelComesBack();
    TestAFailedLoadIsNotALease();
    TestConcurrentRequestsShareOneLoad();
    TestACancelledRequestLoadsNothing();
    TestAFreshlyLoadedModelIsNotImmediatelyEvicted();
    TestAnExplicitReleaseStillRespectsUse();
    TestAGeneratingModelIsNotEvicted();
    TestARequestDuringAnUnloadWaitsForIt();
    TestUnloadedAndFailedStayDistinct();
    std::cout << "Residency tests passed: a model in use is never taken away, and being "
                 "put away is not failing.\n";
}
