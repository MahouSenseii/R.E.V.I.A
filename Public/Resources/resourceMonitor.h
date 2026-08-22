#pragma once

#include "Resources/resourcePlanner.h"
#include "Resources/resourceUsage.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace revia::resources
{

enum class MeterUnit
{
    Mebibytes,
    Threads
};

// One live reading placed next to the budget the plan set aside for it.
//
// Three numbers rather than two, because "5.6 of 6.2" only means something when the
// difference between the budget and the hardware is also visible: a plan that leaves
// 1.5 GiB of VRAM free is doing its job, and a reading that sits under the budget while
// the card is full means something else on the machine took it.
struct UsageMeter
{
    std::string id;
    std::string label;
    MeterUnit unit = MeterUnit::Mebibytes;
    double used = 0.0;
    // What the resource plan allows Revia to consume.
    double budget = 0.0;
    // The physical ceiling the budget was carved out of.
    double capacity = 0.0;
    // False when the platform could not report this reading. A meter that cannot be
    // measured says so; it never falls back to the startup figure, which stopped being
    // true the moment a model loaded.
    bool measured = false;
    std::string detail;

    // Zero when there is no budget to be a fraction of, so a caller can draw a bar
    // without special-casing an unassigned device.
    [[nodiscard]] double BudgetFraction() const;
    [[nodiscard]] bool OverBudget() const;
    // "5.6 / 6.2 GiB budget" or "9.0 / 14 threads".
    [[nodiscard]] std::string Format() const;
};

struct UsageSnapshot
{
    std::vector<UsageMeter> meters;
    // The owned process tree behind the memory and CPU meters, largest first.
    std::vector<ProcessUsage> processes;
    std::chrono::system_clock::time_point sampledAt = std::chrono::system_clock::now();
    bool measured = false;

    [[nodiscard]] std::string Summary() const;
    [[nodiscard]] std::string Detail() const;
};

// Samples what the machine is actually doing and reports it against the immutable plan.
//
// The planner decides placement once at startup and is deliberately not re-run here:
// this monitor observes, and moving a worker because a reading moved would turn a
// reproducible plan into a feedback loop. It owns its own thread rather than borrowing
// the shell's timer, so live usage keeps updating whether or not a window is open.
class ResourceMonitor
{
public:
    using Handler = std::function<void(const UsageSnapshot&)>;

    ResourceMonitor() = default;
    ~ResourceMonitor();

    ResourceMonitor(const ResourceMonitor&) = delete;
    ResourceMonitor& operator=(const ResourceMonitor&) = delete;

    void Start(const ResourcePlan& plan, std::chrono::milliseconds interval, Handler handler);
    void Stop();
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] UsageSnapshot Latest() const;

    // Pure policy: one raw reading becomes budget-relative meters. Separated from the
    // thread and from the platform calls so the arithmetic that the user will read off
    // the screen can be tested against synthetic hardware.
    [[nodiscard]] static UsageSnapshot Compose(
        const ResourcePlan& plan,
        const SystemMemoryReading& memory,
        const std::vector<ProcessUsage>& processes,
        const std::vector<GpuMemoryReading>& gpuReadings,
        bool gpuCountersAvailable,
        std::uint64_t previousOwnedCpuMilliseconds,
        double elapsedSeconds);

    // The CPU thread budget the plan implies: every long-lived worker cap added up.
    [[nodiscard]] static int PlannedThreadBudget(const ResourcePlan& plan);
    [[nodiscard]] static std::uint64_t TotalCpuMilliseconds(
        const std::vector<ProcessUsage>& processes);

private:
    void Run(std::stop_token stopToken);

    ResourcePlan plan;
    std::chrono::milliseconds sampleInterval{2000};
    Handler handler;
    std::jthread worker;
    mutable std::mutex mutex;
    UsageSnapshot latest;
};

} // namespace revia::resources
