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

struct GpuMemoryReading
{
    // The DXGI adapter LUID in the form the performance counters use, so a reading can be
    // tied to an adapter without matching on a display name.
    std::string adapterLuid;
    std::uint64_t dedicatedUsedMiB = 0;
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

// Owns one query over the system-wide dedicated video memory counters.
//
// System-wide is the point: the chat server holds its weights in another process, so a
// per-process video memory figure would report Revia using almost none of the VRAM it is
// actually responsible for. These are the counters Task Manager reads, so the numbers
// here and the numbers the user can check agree.
//
// The query stays open across samples because some counter types need a prior collection
// before they format, and reopening it every couple of seconds would pay that cost
// forever.
class GpuMemorySampler
{
public:
    GpuMemorySampler();
    ~GpuMemorySampler();

    GpuMemorySampler(const GpuMemorySampler&) = delete;
    GpuMemorySampler& operator=(const GpuMemorySampler&) = delete;

    // False when the platform does not expose the counters. Callers must report VRAM as
    // unmeasured rather than substituting the startup reading, which stopped being true
    // the moment a model loaded.
    [[nodiscard]] bool IsAvailable() const;
    [[nodiscard]] std::vector<GpuMemoryReading> Sample();

private:
    void* query = nullptr;
    void* counter = nullptr;
};

} // namespace revia::resources
