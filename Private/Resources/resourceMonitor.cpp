#include "Resources/resourceMonitor.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace revia::resources
{

namespace
{

// Value and unit together, so no caller can pair a converted number with the wrong label.
std::string FormatQuantity(const double mebibytes)
{
    std::ostringstream stream;
    stream << std::fixed;
    if (mebibytes >= 1024.0)
    {
        stream << std::setprecision(1) << (mebibytes / 1024.0) << " GiB";
    }
    else
    {
        stream << std::setprecision(0) << mebibytes << " MiB";
    }
    return stream.str();
}

std::string FormatThreads(const double threads, const bool whole)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(whole ? 0 : 1) << threads;
    return stream.str();
}

double LargestOf(const UsageMeter& meter)
{
    return std::max({meter.used, meter.budget, meter.capacity});
}

// Where a physical resource stops being comfortable.
//
// Judged against the hardware ceiling, never against a budget. Below Elevated the
// machine has room; at High a load that arrives next may not fit; at Critical something
// is about to be refused. The gap between High and Critical is narrow on purpose: on a
// card holding resident model weights the last few percent disappear quickly, and a
// warning that arrives only at the ceiling arrives too late to mean anything.
constexpr double ElevatedAbove = 0.75;
constexpr double HighAbove = 0.90;
constexpr double CriticalAbove = 0.95;
// A resource this far below its ceiling is not merely comfortable, it is unused, and
// saying so is more useful than calling a card at 1% "normal".
constexpr double IdleBelow = 0.05;

std::string FormatPercent(const double fraction)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(0) << (fraction * 100.0) << '%';
    return stream.str();
}

// "NVIDIA GeForce RTX 5070 (CUDA0)". The vendor and product-line words are identical on
// every row of a machine and push the part that identifies the card out of a narrow
// column, so they are dropped; the backend id stays because two identical cards are
// otherwise indistinguishable, and a meter whose label collides with another's is a row
// that overwrites its neighbour.
std::string DeviceIdentity(const GpuDevice& device)
{
    static constexpr const char* VendorPrefixes[] = {
        "NVIDIA GeForce ", "NVIDIA ", "AMD Radeon ", "AMD ", "Intel(R) ", "Intel "
    };
    std::string name = device.name;
    for (const char* prefix : VendorPrefixes)
    {
        const std::string_view candidate(prefix);
        if (name.size() > candidate.size() &&
            std::string_view(name).substr(0, candidate.size()) == candidate)
        {
            name = name.substr(candidate.size());
            break;
        }
    }
    if (name.empty())
    {
        return device.backendId.empty() ? std::string("GPU") : device.backendId;
    }
    return device.backendId.empty() ? name : name + " (" + device.backendId + ")";
}

} // namespace

std::string ToString(const PressureLevel level)
{
    switch (level)
    {
        case PressureLevel::Unmeasured: return "not measured";
        case PressureLevel::Idle: return "Idle";
        case PressureLevel::Normal: return "Normal";
        case PressureLevel::Elevated: return "Elevated";
        case PressureLevel::High: return "High";
        case PressureLevel::Critical: return "Critical";
    }
    return "not measured";
}

double UsageMeter::BudgetFraction() const
{
    if (budget <= 0.0)
    {
        return 0.0;
    }
    return used / budget;
}

double UsageMeter::CapacityFraction() const
{
    if (capacity <= 0.0)
    {
        return 0.0;
    }
    return used / capacity;
}

double UsageMeter::Fraction() const
{
    return basis == MeterBasis::Capacity ? CapacityFraction() : BudgetFraction();
}

bool UsageMeter::OverBudget() const
{
    return measured && budget > 0.0 && used > budget;
}

PressureLevel UsageMeter::Pressure() const
{
    // A budget meter has no physical claim to make. Reporting one would be the exact
    // confusion this type exists to prevent: a card at 88% is not critical because Revia
    // planned to use less of it than she did.
    if (!measured || basis != MeterBasis::Capacity || capacity <= 0.0)
    {
        return PressureLevel::Unmeasured;
    }
    const double fraction = CapacityFraction();
    if (fraction > CriticalAbove) return PressureLevel::Critical;
    if (fraction > HighAbove) return PressureLevel::High;
    if (fraction > ElevatedAbove) return PressureLevel::Elevated;
    if (fraction < IdleBelow) return PressureLevel::Idle;
    return PressureLevel::Normal;
}

std::string UsageMeter::Status() const
{
    if (!measured)
    {
        return "not measured";
    }
    if (basis == MeterBasis::Capacity)
    {
        if (capacity <= 0.0)
        {
            return "no ceiling known";
        }
        // A reading that is already a percentage does not need its own percentage
        // repeated back to it.
        return unit == MeterUnit::Percent
            ? ToString(Pressure())
            : ToString(Pressure()) + " (" + FormatPercent(CapacityFraction()) + " used)";
    }
    if (budget <= 0.0)
    {
        return "no budget set";
    }
    if (OverBudget())
    {
        return "Budget exceeded by " + FormatPercent(BudgetFraction() - 1.0);
    }
    return "Within budget (" + FormatPercent(BudgetFraction()) + ")";
}

std::string UsageMeter::Format() const
{
    if (!measured)
    {
        return "not measured";
    }
    std::ostringstream stream;
    if (unit == MeterUnit::Percent)
    {
        stream << std::fixed << std::setprecision(0) << used << '%';
        return stream.str();
    }
    if (unit == MeterUnit::Threads)
    {
        stream << FormatThreads(used, false);
        if (budget > 0.0)
        {
            stream << " / " << FormatThreads(budget, true) << " threads planned";
        }
        else
        {
            stream << " threads";
        }
        if (capacity > 0.0)
        {
            stream << ", " << FormatThreads(capacity, true) << " logical";
        }
        return stream.str();
    }

    // One unit for the whole line. "512 MiB / 6.2 GiB" makes the reader do the conversion
    // to see how close the two numbers are, which is the only thing the line is for.
    const bool gibibytes = LargestOf(*this) >= 1024.0;
    const char* unitLabel = gibibytes ? " GiB" : " MiB";
    const auto scaled = [gibibytes](const double value)
    {
        const double converted = gibibytes ? value / 1024.0 : value;
        // A reading far below the line's unit still has to be legible. Rendering 40 MiB
        // as "0.0 GiB" beside a 12 GiB budget reads as nothing at all being used.
        const int precision = gibibytes ? (converted < 1.0 ? 2 : 1) : 0;
        std::ostringstream inner;
        inner << std::fixed << std::setprecision(precision) << converted;
        return inner.str();
    };
    stream << scaled(used);
    if (basis == MeterBasis::Capacity)
    {
        // A capacity meter compares with the hardware and stops there. Naming a budget
        // it was never measured against is what made the physical card look overloaded.
        if (capacity > 0.0)
        {
            stream << " / " << scaled(capacity) << unitLabel << " installed";
        }
        else
        {
            stream << unitLabel;
        }
        return stream.str();
    }
    if (budget > 0.0)
    {
        stream << " / " << scaled(budget) << unitLabel << " budget";
    }
    else
    {
        stream << unitLabel;
    }
    if (capacity > 0.0)
    {
        stream << ", " << scaled(capacity) << unitLabel << " installed";
    }
    return stream.str();
}

std::string UsageSnapshot::Summary() const
{
    if (meters.empty())
    {
        return "No live resource readings yet.";
    }
    std::ostringstream stream;
    bool first = true;
    for (const UsageMeter& meter : meters)
    {
        if (!first)
        {
            stream << "; ";
        }
        first = false;
        stream << meter.label << ' ' << meter.Format();
        if (meter.measured)
        {
            stream << " -- " << meter.Status();
        }
    }
    stream << '.';
    return stream.str();
}

std::string UsageSnapshot::Detail() const
{
    std::ostringstream stream;
    stream << Summary();
    for (const UsageMeter& meter : meters)
    {
        if (meter.detail.empty())
        {
            continue;
        }
        stream << "\n  " << meter.label << ": " << meter.detail;
    }
    if (!processes.empty())
    {
        stream << "\n\nOwned processes:";
        for (const ProcessUsage& process : processes)
        {
            stream << "\n  " << process.name << "  "
                << FormatQuantity(static_cast<double>(process.workingSetMiB));
        }
    }
    return stream.str();
}

int ResourceMonitor::PlannedThreadBudget(const ResourcePlan& plan)
{
    return std::max(0, plan.chatCpuThreads) +
        std::max(0, plan.embeddingCpuThreads) +
        std::max(0, plan.speechRecognitionThreads) +
        std::max(0, plan.voiceCpuThreads);
}

std::uint64_t ResourceMonitor::TotalCpuMilliseconds(const std::vector<ProcessUsage>& processes)
{
    std::uint64_t total = 0;
    for (const ProcessUsage& process : processes)
    {
        total += process.cpuTimeMilliseconds;
    }
    return total;
}

UsageSnapshot ResourceMonitor::Compose(
    const ResourcePlan& plan,
    const SystemMemoryReading& memory,
    const std::vector<ProcessUsage>& processes,
    const std::vector<GpuAdapterReading>& gpuReadings,
    const bool gpuCountersAvailable,
    const std::uint64_t previousOwnedCpuMilliseconds,
    const double elapsedSeconds)
{
    UsageSnapshot snapshot;
    snapshot.processes = processes;
    std::sort(snapshot.processes.begin(), snapshot.processes.end(),
        [](const ProcessUsage& left, const ProcessUsage& right)
        {
            return left.workingSetMiB > right.workingSetMiB;
        });

    const auto readingFor = [&gpuReadings](const std::string& luid)
        -> const GpuAdapterReading*
    {
        if (luid.empty())
        {
            return nullptr;
        }
        const auto found = std::find_if(gpuReadings.begin(), gpuReadings.end(),
            [&luid](const GpuAdapterReading& reading)
            {
                return reading.adapterLuid == luid;
            });
        return found == gpuReadings.end() ? nullptr : &*found;
    };

    const auto placedOn = [&plan](const GpuDevice& device)
    {
        std::vector<std::string> workloads;
        const auto matches = [&device](const std::string& configured)
        {
            if (configured.empty() || device.backendId.empty())
            {
                return false;
            }
            return configured == device.backendId || configured == device.QwenDevice();
        };
        if (std::any_of(plan.chatGpus.begin(), plan.chatGpus.end(),
                [&device](const GpuDevice& assigned)
                {
                    return assigned.backendId == device.backendId;
                }))
        {
            workloads.emplace_back("chat/vision");
        }
        if (matches(plan.voiceDevice)) workloads.emplace_back("voice");
        if (matches(plan.speechRecognitionDevice)) workloads.emplace_back("speech recognition");
        if (matches(plan.embeddingDevice)) workloads.emplace_back("embeddings");
        return workloads;
    };

    for (const GpuDevice& device : plan.hardware.gpus)
    {
        const std::string base =
            "gpu:" + (device.backendId.empty() ? device.name : device.backendId);
        const std::string identity = DeviceIdentity(device);
        const GpuAdapterReading* reading = readingFor(device.adapterLuid);
        const std::vector<std::string> workloads = placedOn(device);

        // Why the reading is missing, said once and reused: three meters that disagree
        // about whether a card could be read would be three bugs waiting to happen.
        const std::string unreadable = device.adapterLuid.empty()
            ? "The backend device could not be matched to a display adapter."
            : gpuCountersAvailable
                ? "The platform reported no counter for this adapter."
                : "The GPU performance counters are not available.";

        std::ostringstream placement;
        placement << (device.name.empty() ? std::string("This adapter") : device.name);
        if (workloads.empty())
        {
            placement << " -- no Revia workload was placed here.";
        }
        else
        {
            placement << " -- running";
            for (std::size_t index = 0; index < workloads.size(); ++index)
            {
                placement << (index == 0 ? " " : ", ") << workloads[index];
            }
            placement << '.';
        }

        // What the card itself is doing. This is the number that answers "is the GPU in
        // trouble", and it is judged against the hardware, never against Revia's plan.
        UsageMeter vram;
        vram.id = base + ":vram";
        vram.label = identity + " VRAM";
        vram.unit = MeterUnit::Mebibytes;
        vram.basis = MeterBasis::Capacity;
        vram.capacity = static_cast<double>(device.totalMemoryMiB);
        if (reading != nullptr && reading->memoryMeasured)
        {
            vram.used = static_cast<double>(reading->dedicatedUsedMiB);
            vram.measured = true;
        }
        vram.detail = placement.str() + ' ' + (vram.measured
            ? "System-wide dedicated usage for the whole card, so other applications are "
              "included; this is the figure Task Manager shows."
            : "Live video memory is unavailable. " + unreadable);
        snapshot.meters.push_back(std::move(vram));

        // What Revia planned for. Separate from the card's own reading because exceeding
        // an allowance she set for herself is a planning result, not a hardware fault:
        // the same occupancy can be a healthy card and an over-subscribed plan at once.
        UsageMeter budget;
        budget.id = base + ":budget";
        budget.label = identity + " Revia budget";
        budget.unit = MeterUnit::Mebibytes;
        budget.basis = MeterBasis::Budget;
        // The plan's promise for a device is that this much stays free on it, so the
        // budget is a ceiling on total occupancy rather than on Revia's own share --
        // which is also the only thing the counters can report, since the weights live
        // in a child process.
        budget.budget = std::max(0.0,
            static_cast<double>(device.totalMemoryMiB) -
                static_cast<double>(std::max(0, plan.gpuReserveMiB)));
        if (reading != nullptr && reading->memoryMeasured)
        {
            budget.used = static_cast<double>(reading->dedicatedUsedMiB);
            budget.measured = true;
        }
        {
            std::ostringstream detail;
            detail << "The plan promises to leave "
                << FormatQuantity(static_cast<double>(std::max(0, plan.gpuReserveMiB)))
                << " free on any device it uses. Total occupancy is measured against "
                   "that, so memory another application took counts toward it.";
            if (!budget.measured)
            {
                detail << ' ' << unreadable;
            }
            budget.detail = detail.str();
        }
        snapshot.meters.push_back(std::move(budget));

        UsageMeter compute;
        compute.id = base + ":compute";
        compute.label = identity + " compute";
        compute.unit = MeterUnit::Percent;
        compute.basis = MeterBasis::Capacity;
        compute.capacity = 100.0;
        if (reading != nullptr && reading->utilizationMeasured)
        {
            compute.used = reading->utilizationPercent;
            compute.measured = true;
        }
        compute.detail = compute.measured
            ? "Busiest engine on the adapter. Memory can be nearly full while compute is "
              "idle: resident model weights occupy VRAM whether or not anything is "
              "generating."
            : (reading == nullptr
                ? "Live engine utilisation is unavailable. " + unreadable
                : std::string("The GPU engine utilisation counter is not available on "
                    "this machine."));
        snapshot.meters.push_back(std::move(compute));
    }

    UsageMeter ram;
    ram.id = "ram";
    ram.label = "RAM";
    ram.unit = MeterUnit::Mebibytes;
    ram.capacity = static_cast<double>(plan.hardware.totalSystemMemoryMiB);
    ram.budget = std::max(0.0,
        static_cast<double>(plan.hardware.totalSystemMemoryMiB) -
            static_cast<double>(std::max(0, plan.reservedSystemMemoryMiB)));
    if (!processes.empty())
    {
        std::uint64_t owned = 0;
        for (const ProcessUsage& process : processes)
        {
            owned += process.workingSetMiB;
        }
        ram.used = static_cast<double>(owned);
        ram.measured = true;
    }
    {
        std::ostringstream detail;
        detail << "Revia and every process it started";
        if (memory.measured && memory.totalMiB > 0)
        {
            const std::uint64_t systemUsed = memory.totalMiB > memory.availableMiB
                ? memory.totalMiB - memory.availableMiB
                : 0;
            detail << ". The machine as a whole is using "
                << FormatQuantity(static_cast<double>(systemUsed)) << " of "
                << FormatQuantity(static_cast<double>(memory.totalMiB));
        }
        detail << '.';
        ram.detail = detail.str();
    }
    snapshot.meters.push_back(std::move(ram));

    UsageMeter cpu;
    cpu.id = "cpu";
    cpu.label = "CPU worker load";
    cpu.unit = MeterUnit::Threads;
    cpu.capacity = static_cast<double>(plan.hardware.logicalProcessors);
    cpu.budget = static_cast<double>(PlannedThreadBudget(plan));
    const std::uint64_t currentCpu = TotalCpuMilliseconds(processes);
    if (elapsedSeconds > 0.0 && currentCpu >= previousOwnedCpuMilliseconds &&
        previousOwnedCpuMilliseconds > 0)
    {
        // Processor time consumed per second of wall clock is the number of threads that
        // were actually busy, which is the question a thread cap is an answer to. A raw
        // thread count would report the pool llama.cpp created, not the work it did.
        const double busy =
            static_cast<double>(currentCpu - previousOwnedCpuMilliseconds) /
            (elapsedSeconds * 1000.0);
        cpu.used = std::max(0.0, busy);
        cpu.measured = true;
    }
    cpu.detail = cpu.measured
        ? "Processor time consumed per second across the owned processes, against the "
          "chat, embedding, speech-recognition, and voice thread caps the plan set."
        : "Waiting for a second sample; load is a difference between two readings.";
    snapshot.meters.push_back(std::move(cpu));

    snapshot.measured = std::any_of(snapshot.meters.begin(), snapshot.meters.end(),
        [](const UsageMeter& meter) { return meter.measured; });
    return snapshot;
}

ResourceMonitor::~ResourceMonitor()
{
    Stop();
}

void ResourceMonitor::Start(
    const ResourcePlan& inputPlan,
    const std::chrono::milliseconds interval,
    Handler inputHandler)
{
    Stop();
    {
        std::lock_guard lock(mutex);
        plan = inputPlan;
        sampleInterval = interval.count() > 0 ? interval : std::chrono::milliseconds(2000);
        handler = std::move(inputHandler);
        latest = UsageSnapshot{};
    }
    worker = std::jthread([this](const std::stop_token stopToken) { Run(stopToken); });
}

void ResourceMonitor::Stop()
{
    if (worker.joinable())
    {
        worker.request_stop();
        worker.join();
    }
}

bool ResourceMonitor::IsRunning() const
{
    return worker.joinable();
}

UsageSnapshot ResourceMonitor::Latest() const
{
    std::lock_guard lock(mutex);
    return latest;
}

void ResourceMonitor::Run(const std::stop_token stopToken)
{
    // Created on the worker thread so an unavailable counter set costs the caller
    // nothing at startup and is retried on the next run rather than on every sample.
    GpuAdapterSampler gpuSampler;

    ResourcePlan localPlan;
    std::chrono::milliseconds interval{2000};
    Handler localHandler;
    {
        std::lock_guard lock(mutex);
        localPlan = plan;
        interval = sampleInterval;
        localHandler = handler;
    }

    std::mutex sleepMutex;
    std::condition_variable_any sleepCondition;
    std::uint64_t previousCpu = 0;
    auto previousAt = std::chrono::steady_clock::now();
    bool first = true;

    while (!stopToken.stop_requested())
    {
        const auto now = std::chrono::steady_clock::now();
        const double elapsedSeconds = first
            ? 0.0
            : std::chrono::duration<double>(now - previousAt).count();

        const std::vector<ProcessUsage> processes = SampleOwnedProcesses();
        UsageSnapshot snapshot = Compose(
            localPlan,
            SampleSystemMemory(),
            processes,
            gpuSampler.Sample(),
            gpuSampler.IsAvailable(),
            previousCpu,
            elapsedSeconds);

        previousCpu = TotalCpuMilliseconds(processes);
        previousAt = now;
        first = false;

        {
            std::lock_guard lock(mutex);
            latest = snapshot;
        }
        if (localHandler)
        {
            localHandler(snapshot);
        }

        std::unique_lock sleepLock(sleepMutex);
        sleepCondition.wait_for(sleepLock, stopToken, interval,
            [&stopToken] { return stopToken.stop_requested(); });
    }
}

} // namespace revia::resources
