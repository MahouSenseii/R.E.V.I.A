#include "testSupport.h"

#include "Core/configManager.h"
#include "Core/messageRouter.h"
#include "Intelligence/modelLifetime.h"
#include "LLM/LLamaCPP/llamaCppServerProcess.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// On-demand residency, measured on the cards rather than asserted.
//
//   ReviaTests.exe --residency-live
//
// `residencyTests.cpp` already covers the coordinator's policy -- leases, idle grace,
// minimum residency, coalesced loads, no eviction while in use -- and covers it without
// a model, which is right: that is control flow and it should be testable in
// milliseconds.
//
// What that cannot show is whether putting a model away actually gives the memory back.
// A coordinator can mark a role Unloaded, report it cheerfully, and leave five gigabytes
// resident because the process is still alive. The state is a label; the card is the
// fact. So this starts a real llama.cpp server through the same process owner the
// session uses, drives the real coordinator, and reads the adapters before and after
// each transition.
//
// It owns every process it starts and stops only those.

namespace
{

using namespace revia::intelligence;
using revia::tests::Check;

struct DeviceMemory
{
    int index = 0;
    std::string name;
    std::uint64_t totalMiB = 0;
    std::uint64_t usedMiB = 0;
};

// What the cards report, right now.
//
// Read through nvidia-smi rather than through the runtime's own sampler on purpose:
// this is the number being used to check the runtime's claims, and taking it from the
// thing under test would make the check circular. It is also the figure a person can
// reproduce by hand while watching the run.
std::vector<DeviceMemory> ReadDevices()
{
    std::vector<DeviceMemory> devices;
#ifdef _WIN32
    FILE* pipe = _popen(
        "nvidia-smi --query-gpu=index,name,memory.total,memory.used "
        "--format=csv,noheader,nounits 2>NUL", "r");
#else
    FILE* pipe = popen(
        "nvidia-smi --query-gpu=index,name,memory.total,memory.used "
        "--format=csv,noheader,nounits 2>/dev/null", "r");
#endif
    if (pipe == nullptr) return devices;

    char line[512];
    while (std::fgets(line, sizeof(line), pipe) != nullptr)
    {
        std::istringstream stream(line);
        std::string index, name, total, used;
        if (!std::getline(stream, index, ',')) continue;
        if (!std::getline(stream, name, ',')) continue;
        if (!std::getline(stream, total, ',')) continue;
        if (!std::getline(stream, used, ',')) continue;

        const auto trim = [](std::string value)
        {
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return std::string();
            const auto last = value.find_last_not_of(" \t\r\n");
            return value.substr(first, last - first + 1);
        };

        DeviceMemory device;
        try
        {
            device.index = std::stoi(trim(index));
            device.name = trim(name);
            device.totalMiB = std::stoull(trim(total));
            device.usedMiB = std::stoull(trim(used));
        }
        catch (const std::exception&)
        {
            continue;
        }
        devices.push_back(std::move(device));
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return devices;
}

void PrintDevices(const std::string& label, const std::vector<DeviceMemory>& devices)
{
    std::cout << "  " << std::left << std::setw(26) << label;
    for (const DeviceMemory& device : devices)
    {
        std::cout << "  gpu" << device.index << "=" << std::setw(5) << std::right
                  << device.usedMiB << "MiB";
    }
    std::cout << "\n";
}

// Settling matters. A process that has exited has not necessarily had its memory
// reclaimed by the driver at the instant the handle closes, and reading too early
// reports a saving that has not happened yet -- or worse, reports none that has.
std::vector<DeviceMemory> ReadAfterSettling(const int milliseconds = 2500)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    return ReadDevices();
}

std::uint64_t TotalUsed(const std::vector<DeviceMemory>& devices)
{
    std::uint64_t total = 0;
    for (const DeviceMemory& device : devices) total += device.usedMiB;
    return total;
}

bool ServerAnswers(const llmSettings& settings)
{
    messageRouter router;
    llmSettings probe = settings;
    probe.bAutoStartServer = false;
    router.ApplyLLMSettings(probe, embeddingSettings{}, aiProfile{});
    return router.CheckLLMHealth().bIsAvailable;
}

bool WaitForServer(const llmSettings& settings, const int seconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (ServerAnswers(settings)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

} // namespace

void RunResidencyLive()
{
    std::cout << "\n========== On-demand residency, measured ==========\n";

    const std::vector<DeviceMemory> devices = ReadDevices();
    if (devices.empty())
    {
        std::cout << "\nNOT MEASURED\n";
        std::cout << "  reason: nvidia-smi reported no devices, so there is nothing to "
                     "measure memory on.\n";
        std::cout << "  The coordinator's policy is covered without hardware by "
                     "--residency; what is missing here is the card.\n";
        return;
    }

    std::cout << "\nAdapters:\n";
    for (const DeviceMemory& device : devices)
    {
        std::cout << "  gpu" << device.index << "  " << device.name << "  "
                  << device.totalMiB << "MiB total\n";
    }

    appSettings configured;
    configManager config;
    Check(config.LoadSettings(configured), "Config/settings.json could not be loaded.");

    // The Expert tier, on a port this run owns. Deliberately not the configured port:
    // a server already running there belongs to somebody else, and this test must not
    // stop it -- which is the same rule WasStartedByRevia enforces in the session.
    llmSettings expert;
    expert.backend = "LLamaCpp";
    expert.host = "127.0.0.1";
    expert.port = 8097;
    expert.modelName = configured.intelligence.expert.modelName;
    expert.modelPath = configured.intelligence.expert.modelPath;
    expert.contextSize = 4096;
    expert.parallelRequests = 1;
    expert.bAutoStartServer = true;
    expert.bVisionEnabled = false;
    expert.startupTimeoutSeconds = 180;
    // Taken from the configured settings rather than left at the default. The default
    // is relative and resolves against the runtime root, which is the repository when
    // the application runs and the build directory when a test does -- and the test
    // binary lives one tree away from ThirdParty.
    expert.serverExecutable = configured.llm.serverExecutable;

    std::cout << "\nExpert model: " << expert.modelPath << "\n";
    std::cout << "Port:         " << expert.port
              << "  (this run's own; a server on the configured port is left alone)\n";

    // The launch configuration, printed because without it the per-device split below
    // is a number with no explanation.
    //
    // Nothing here sets a device split, a tensor split or a layer count. llama.cpp is
    // left to place layers itself across whatever CUDA devices it finds, which is what
    // the application does -- so what the cards show is real behaviour rather than an
    // arrangement invented for the measurement. It also means the split is not
    // reproducible by assertion: it is whatever the runtime chose on this machine today,
    // and the report has to say that rather than present it as a property of the design.
    const char* visible = std::getenv("CUDA_VISIBLE_DEVICES");
    std::cout << "\nLaunch configuration (what explains the per-device split below)\n";
    std::cout << "  server:          " << expert.serverExecutable << "\n";
    std::cout << "  context:         " << expert.contextSize << " tokens\n";
    std::cout << "  parallel slots:  " << expert.parallelRequests << "\n";
    std::cout << "  vision:          " << (expert.bVisionEnabled ? "on" : "off") << "\n";
    std::cout << "  device split:    not specified -- llama.cpp places layers itself\n";
    std::cout << "  visible devices: "
              << (visible == nullptr ? std::string("unset (all)") : std::string(visible))
              << "\n";

    if (ServerAnswers(expert))
    {
        std::cout << "\nNOT MEASURED\n";
        std::cout << "  reason: something is already serving on port " << expert.port
                  << ". This test will not stop a process it did not start.\n";
        return;
    }

    // The real process owner and the real coordinator, wired the way the session wires
    // them. Measuring a stand-in would measure the stand-in.
    llamaCppServerProcess process;
    ModelResidencyManager inventory;
    ModelResidency expertResidency;
    expertResidency.tier = IntelligenceTier::Expert;
    expertResidency.role = "Expert";
    expertResidency.state = ResidencyState::Unloaded;
    inventory.Register(expertResidency);

    ModelLifetimeCoordinator coordinator(inventory);
    std::atomic<int> loads{0};
    std::atomic<int> unloads{0};
    std::atomic<long long> lastLoadMs{0};

    ModelLifetimePolicy policy;
    policy.onDemand = true;
    // Short, because this run is deliberately trying to provoke a thrash. In the
    // application these are minutes.
    policy.idleGraceMs = 1000;
    policy.minimumResidencyMs = 0;

    coordinator.Manage(IntelligenceTier::Expert,
        [&](std::stop_token token) -> bool
        {
            const auto started = std::chrono::steady_clock::now();
            std::string error;
            if (!process.Start(expert, error))
            {
                std::cout << "  load failed: " << error << "\n";
                return false;
            }
            if (!WaitForServer(expert, expert.startupTimeoutSeconds) ||
                token.stop_requested())
            {
                process.Stop();
                return false;
            }
            lastLoadMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count());
            ++loads;
            return true;
        },
        [&]()
        {
            // The same guard the session uses. A server this run did not start is not
            // this run's to stop, whatever the coordinator decides about residency.
            if (process.WasStartedByRevia())
            {
                process.Stop();
                ++unloads;
            }
        },
        policy);

    const std::vector<DeviceMemory> baseline = ReadDevices();
    PrintDevices("before loading Expert", baseline);

    // ---- load ----
    std::cout << "\n-- acquiring Expert --\n";
    std::vector<DeviceMemory> loaded;
    std::uint64_t loadedTotal = 0;
    {
        ModelLifetimeCoordinator::Lease lease = coordinator.Acquire(IntelligenceTier::Expert);
        Check(static_cast<bool>(lease),
            "Expert could not be brought up, so there is nothing to measure.");
        std::cout << "  cold start:               " << lastLoadMs.load() << "ms\n";
        loaded = ReadAfterSettling();
        loadedTotal = TotalUsed(loaded);
        PrintDevices("Expert loaded, in use", loaded);

        // An idle sweep must not take a model out from under a live lease. This is the
        // case where a label and a fact come apart most expensively: the caller is
        // mid-request and the process disappears.
        coordinator.SweepIdle();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        coordinator.SweepIdle();
        Check(process.IsRunning(),
            "The idle sweep stopped a model that a lease was still holding.");
        Check(unloads.load() == 0, "A held model was put away.");
        std::cout << "  swept twice while held:   still running (correct)\n";
    }

    // ---- idle, then swept ----
    std::cout << "\n-- lease released, waiting out the idle grace --\n";
    const std::vector<DeviceMemory> idle = ReadDevices();
    PrintDevices("released, not yet swept", idle);
    Check(process.IsRunning(),
        "Releasing the lease stopped the process immediately, which is not what an "
        "idle grace is for.");

    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    coordinator.SweepIdle();
    Check(unloads.load() == 1,
        "The idle sweep did not put Expert away after its grace expired.");
    Check(!process.IsRunning(), "The process outlived the unload.");

    const std::vector<DeviceMemory> unloaded = ReadAfterSettling();
    const std::uint64_t unloadedTotal = TotalUsed(unloaded);
    PrintDevices("after unloading", unloaded);

    // ---- the number that matters ----
    const std::uint64_t baselineTotal = TotalUsed(baseline);
    const std::uint64_t held = loadedTotal > baselineTotal ? loadedTotal - baselineTotal : 0;
    const std::uint64_t freed = loadedTotal > unloadedTotal ? loadedTotal - unloadedTotal : 0;

    std::cout << "\n-- what the cards say --\n";

    // Per device first, then the aggregate.
    //
    // The aggregate on its own hides the thing a reader most needs: which card actually
    // took the model. Two adapters that each moved by half is a very different machine
    // from one that took all of it, and an "on-demand residency" claim about the second
    // says nothing about the first.
    std::cout << "  per device (used MiB: before -> loaded -> unloaded)\n";
    for (std::size_t index = 0; index < baseline.size(); ++index)
    {
        const DeviceMemory& before = baseline[index];
        const std::int64_t start = static_cast<std::int64_t>(before.usedMiB);
        const std::int64_t after = index < loaded.size()
            ? static_cast<std::int64_t>(loaded[index].usedMiB) : start;
        const std::int64_t ended = index < unloaded.size()
            ? static_cast<std::int64_t>(unloaded[index].usedMiB) : start;
        std::cout << "    gpu" << before.index << "  " << std::setw(6) << std::right
                  << start << " -> " << std::setw(6) << after << " -> "
                  << std::setw(6) << ended
                  << "   held " << std::showpos << (after - start)
                  << ", residual " << (ended - start) << std::noshowpos << "\n";
    }

    std::cout << "  aggregate\n";
    std::cout << "    Expert held:            " << held << "MiB\n";
    std::cout << "    released on unload:     " << freed << "MiB\n";

    // Signed, and not clamped.
    //
    // The previous report said "0 MiB residual", and the raw figures it was built from
    // were 4,008 -> 3,997 on one card: the machine ended eleven megabytes *below* where
    // it started, because other applications moved while this ran. Clamping that to zero
    // turned measurement noise into a claim of exactness. The honest statement is that
    // the end state is approximately the baseline, with the raw difference printed so a
    // reader can judge the approximation for themselves.
    const std::int64_t residual = static_cast<std::int64_t>(unloadedTotal) -
        static_cast<std::int64_t>(baselineTotal);
    std::cout << "    residual vs baseline:   " << std::showpos << residual
              << std::noshowpos << "MiB  (raw, unclamped)\n";
    // A tenth of what the model held, or 128MiB, whichever is larger. Other processes
    // move memory while this runs; a window opening is not a leak.
    const std::int64_t noiseFloor =
        std::max<std::int64_t>(128, static_cast<std::int64_t>(held / 10));
    const bool approximatelyBaseline = residual < noiseFloor && residual > -noiseFloor;
    std::cout << "    reading:                "
              << (approximatelyBaseline
                    ? "approximately returned to baseline (within " +
                        std::to_string(noiseFloor) + "MiB of it)"
                    : "NOT returned to baseline")
              << "\n";

    Check(held > 256,
        "Loading Expert did not move video memory measurably, so either it did not "
        "load or it is not on a card this can read -- and either way the numbers below "
        "would mean nothing.");
    // The assertion this whole file exists for. A state change that frees nothing is a
    // label, and a label is not a saving.
    Check(freed > held / 2,
        "Expert reported itself unloaded and the card did not give the memory back: "
        "held " + std::to_string(held) + "MiB, released " + std::to_string(freed) +
            "MiB. An Unloaded state is not released VRAM.");

    // ---- reload ----
    std::cout << "\n-- reacquiring --\n";
    {
        ModelLifetimeCoordinator::Lease lease = coordinator.Acquire(IntelligenceTier::Expert);
        Check(static_cast<bool>(lease), "Expert did not come back after being put away.");
        Check(loads.load() == 2, "The reload did not go through the activator.");
        std::cout << "  cold start (reload):      " << lastLoadMs.load() << "ms\n";
        const std::vector<DeviceMemory> reloaded = ReadAfterSettling();
        PrintDevices("reloaded", reloaded);
        const std::uint64_t reloadedHeld = TotalUsed(reloaded) > baselineTotal
            ? TotalUsed(reloaded) - baselineTotal : 0;
        std::cout << "  held again:               " << reloadedHeld << "MiB\n";
    }

    // ---- thrash ----
    //
    // A sweep that runs often must not turn into a load/unload cycle. Repeated sweeps
    // with no acquisition in between should do nothing at all after the first.
    std::cout << "\n-- repeated sweeps --\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    for (int sweep = 0; sweep < 5; ++sweep)
    {
        coordinator.SweepIdle();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cout << "  loads: " << loads.load() << "   unloads: " << unloads.load() << "\n";
    Check(loads.load() == 2 && unloads.load() == 2,
        "Repeated sweeps caused reload thrashing: " + std::to_string(loads.load()) +
            " load(s) and " + std::to_string(unloads.load()) + " unload(s).");

    // ---- cancellation ----
    std::cout << "\n-- cancelled acquisition --\n";
    {
        std::stop_source source;
        source.request_stop();
        ModelLifetimeCoordinator::Lease lease =
            coordinator.Acquire(IntelligenceTier::Expert, source.get_token());
        Check(!lease, "A cancelled acquisition still produced a lease.");
        Check(loads.load() == 2,
            "A cancelled acquisition started a load anyway; loads reached " +
                std::to_string(loads.load()) + ".");
        std::cout << "  no lease, no load (correct)\n";
    }

    // ---- a server this run does not own ----
    std::cout << "\n-- a server Revia did not start --\n";
    {
        llamaCppServerProcess foreign;
        Check(!foreign.WasStartedByRevia(),
            "A process object that started nothing claimed ownership.");
        foreign.Stop();
        std::cout << "  Stop() on an unowned process did nothing (correct)\n";
    }

    // ---- repeated cold and warm cycles ----
    //
    // One reload is an anecdote. What a person actually experiences is a sequence:
    // ask, wait for a cold start, ask again straight away and get a warm one, leave it
    // alone, come back to another cold start. The costs of those are different by three
    // orders of magnitude and both of them matter.
    std::cout << "\n-- repeated cold and warm cycles --\n";
    std::vector<long long> coldStarts;
    std::vector<long long> warmAcquires;
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        const auto coldStarted = std::chrono::steady_clock::now();
        {
            ModelLifetimeCoordinator::Lease lease =
                coordinator.Acquire(IntelligenceTier::Expert);
            Check(static_cast<bool>(lease),
                "Expert did not come back on cycle " + std::to_string(cycle + 1) + ".");
            coldStarts.push_back(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - coldStarted).count());

            // Warm: the model is already up and the lease is already held. This is the
            // path an interactive turn takes, and it is the one that must not stall.
            const auto warmStarted = std::chrono::steady_clock::now();
            ModelLifetimeCoordinator::Lease second =
                coordinator.Acquire(IntelligenceTier::Expert);
            warmAcquires.push_back(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - warmStarted).count());
            Check(static_cast<bool>(second),
                "A second lease on an already-resident model was refused.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1400));
        coordinator.SweepIdle();
    }
    for (std::size_t cycle = 0; cycle < coldStarts.size(); ++cycle)
    {
        std::cout << "  cycle " << (cycle + 1) << ":  cold "
                  << std::setw(6) << std::right << coldStarts[cycle] << "ms    warm "
                  << std::setw(6) << warmAcquires[cycle] << "us\n";
    }
    const long long worstWarm =
        *std::max_element(warmAcquires.begin(), warmAcquires.end());
    std::cout << "  worst warm acquire:       " << worstWarm << "us\n";
    // The responsiveness question this file can actually answer. A warm acquire is what
    // sits in front of an interactive turn, and if it were measured in hundreds of
    // milliseconds a voice reply would audibly wait on it.
    Check(worstWarm < 50000,
        "A warm acquire took " + std::to_string(worstWarm) + "us. An interactive turn "
        "sits behind this, so anything near a tenth of a second is audible.");
    std::cout << "  reading:                  a warm acquire does not stall a turn.\n";
    std::cout << "  NOT measured here:        end-to-end speech latency while a tier is\n"
                 "                            actually loading. That needs the speech\n"
                 "                            worker and a clock on the audio, and this\n"
                 "                            file has neither.\n";

    // ---- is the shipped configuration actually on-demand? ----
    //
    // Everything above measures a coordinator this test configured. What a user gets is
    // decided by settings.json, and the two have no reason to agree.
    std::cout << "\n-- the configuration a user actually runs --\n";
    const auto describeTier = [](const char* name, const modelTierSettings& tier)
    {
        std::cout << "  " << std::left << std::setw(10) << name
                  << "on_demand=" << (tier.bOnDemand ? "yes" : "no")
                  << "  warm_at_startup=" << (tier.bWarmAtStartup ? "yes" : "no")
                  << "  idle_grace=" << tier.idleGraceSeconds << "s"
                  << "  min_residency=" << tier.minimumResidencySeconds << "s\n";
    };
    describeTier("fast", configured.intelligence.fast);
    describeTier("expert", configured.intelligence.expert);
    if (!configured.intelligence.expert.bOnDemand)
    {
        std::cout << "  reading:  Expert is NOT on-demand in the shipped settings, so "
                     "none of the\n            savings measured above are savings this "
                     "machine is currently taking.\n            The mechanism works; the "
                     "switch is off.\n";
    }
    else
    {
        std::cout << "  reading:  Expert is on-demand in the shipped settings, so the "
                     "behaviour measured\n            above is the behaviour a user "
                     "gets.\n";
    }

    const std::vector<DeviceMemory> finalState = ReadAfterSettling();
    PrintDevices("at the end", finalState);

    std::cout << "\nMeasured: Expert held " << held << "MiB and gave back " << freed
              << "MiB when put away. Cold start " << lastLoadMs.load() << "ms.\n";
    std::cout << "Every process this run started, it stopped. Nothing else was touched.\n";
}
