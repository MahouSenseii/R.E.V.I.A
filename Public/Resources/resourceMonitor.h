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
    Threads,
    // Already a percentage of its own device, such as GPU engine utilisation.
    Percent
};

// What a meter's percentage is a fraction of.
//
// The distinction is the difference between two questions a user asks separately: is the
// hardware in trouble, and is Revia inside the allowance her plan set. A card 88% full
// while Revia is 7% past a budget carved out of it is one healthy number and one
// planning number, and a single bar reading 107% claims the first is critical when it is
// not.
enum class MeterBasis
{
    Budget,
    Capacity
};

// How hard a physical resource is being pressed, judged against its own ceiling.
//
// Deliberately about the hardware only. A budget overrun is a separate statement made in
// the meter's own terms, because exceeding an allowance Revia set for herself is not the
// same event as running a card out of memory.
enum class PressureLevel
{
    Unmeasured,
    Idle,
    Normal,
    Elevated,
    High,
    Critical
};

[[nodiscard]] std::string ToString(PressureLevel level);

// One live reading placed next to the ceiling it should be judged against.
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
    MeterBasis basis = MeterBasis::Budget;
    double used = 0.0;
    // What the resource plan allows Revia to consume. Left at zero on a capacity meter,
    // which has no allowance of its own to exceed -- and which is also what keeps the
    // load governor reading each device once rather than once per view of it.
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
    [[nodiscard]] double CapacityFraction() const;
    // The fraction the meter's own basis makes it a statement about. This is the number
    // a bar should draw, so the bar and the label can never disagree.
    [[nodiscard]] double Fraction() const;
    [[nodiscard]] bool OverBudget() const;
    // Unmeasured on a budget meter: physical pressure is a claim about hardware, and a
    // budget is not hardware.
    [[nodiscard]] PressureLevel Pressure() const;
    // "88% used", "Budget exceeded by 7%", "Idle".
    [[nodiscard]] std::string Status() const;
    // "5.6 / 6.2 GiB budget", "10.6 / 12.0 GiB installed" or "9.0 / 14 threads".
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
        const std::vector<GpuAdapterReading>& gpuReadings,
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
