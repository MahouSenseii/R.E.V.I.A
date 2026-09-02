#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace revia::resources
{

// One process in the tree Revia owns: this process, or something it started.
struct ProcessUsage
{
    std::uint32_t processId = 0;
    std::string name;
    std::uint64_t workingSetMiB = 0;
    // Kernel plus user time since the process started. Only differences between two
    // samples mean anything; the absolute value is startup history.
    std::uint64_t cpuTimeMilliseconds = 0;
};

struct SystemMemoryReading
{
    std::uint64_t totalMiB = 0;
    std::uint64_t availableMiB = 0;
    bool measured = false;
};

// One performance-counter instance name, reduced to the adapter it belongs to.
//
// The memory and engine counters name the same card differently -- the engine counter
// adds the owning process and the engine -- and readings from the two are useless unless
// both reduce to the same key. Exposed rather than kept private because that agreement
// is the whole basis for attributing a reading to a device, and it is worth a test.
struct GpuCounterInstance
{
    // "luid_0x00000000_0x0000f338", or empty when the name carries no adapter.
    std::string adapterKey;
    // "3d", "copy", "videodecode"; empty for counters that are not per-engine.
    std::string engineType;
};

[[nodiscard]] GpuCounterInstance ParseGpuCounterInstance(const std::string& instanceName);

struct GpuAdapterReading
{
    // The DXGI adapter LUID in the form the performance counters use, so a reading can be
    // tied to an adapter without matching on a display name.
    std::string adapterLuid;
    std::uint64_t dedicatedUsedMiB = 0;
    bool memoryMeasured = false;
    // The busiest engine on the adapter, 0..100, which is what the Task Manager GPU
    // column reports.
    //
    // Occupancy and activity answer different questions and must not be conflated. A card
    // holding resident model weights while nothing is being generated reads nearly full
    // on memory and idle on compute, and a panel that shows only the first makes an
    // untouched GPU look overloaded.
    double utilizationPercent = 0.0;
    bool utilizationMeasured = false;
};

// Every process Revia started, transitively, plus Revia itself.
//
// Walking the tree rather than asking each owner for its handle is deliberate. The chat
// server, the embedding server, the voice worker, and whisper bursts are all children of
// this process, so the tree is the definition of "owned" and stays correct when a worker
// is added later. A registry of handles would have to be updated by hand and would be
// wrong in exactly the case that matters -- a worker somebody forgot to register.
[[nodiscard]] std::vector<ProcessUsage> SampleOwnedProcesses();

[[nodiscard]] SystemMemoryReading SampleSystemMemory();

[[nodiscard]] unsigned int LogicalProcessorCount();

// Owns one query over the system-wide per-adapter GPU counters: dedicated video memory
// and engine utilisation.
//
// System-wide is the point: the chat server holds its weights in another process, so a
// per-process video memory figure would report Revia using almost none of the VRAM it is
// actually responsible for. These are the counters Task Manager reads, so the numbers
// here and the numbers the user can check agree.
//
// One query for both counters rather than two samplers, because they are the same
// question asked of the same adapters at the same instant, and two independent
// collections would report memory and compute from moments that do not line up.
//
// The query stays open across samples because some counter types need a prior collection
// before they format, and reopening it every couple of seconds would pay that cost
// forever.
class GpuAdapterSampler
{
public:
    GpuAdapterSampler();
    ~GpuAdapterSampler();

    GpuAdapterSampler(const GpuAdapterSampler&) = delete;
    GpuAdapterSampler& operator=(const GpuAdapterSampler&) = delete;

    // False when the platform does not expose the memory counters. Callers must report
    // VRAM as unmeasured rather than substituting the startup reading, which stopped
    // being true the moment a model loaded.
    [[nodiscard]] bool IsAvailable() const;
    // Utilisation is reported separately because it can be missing on a machine whose
    // memory counters work. Losing the compute reading must not cost the memory one.
    [[nodiscard]] bool IsUtilizationAvailable() const;
    [[nodiscard]] std::vector<GpuAdapterReading> Sample();

private:
    void* query = nullptr;
    void* memoryCounter = nullptr;
    void* utilizationCounter = nullptr;
};

} // namespace revia::resources
