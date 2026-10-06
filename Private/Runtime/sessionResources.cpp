#include "Runtime/reviaSession.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace revia::runtime
{

void ReviaSession::PublishResourcePlan() const
{
    std::size_t gpuIndex = 0;
    for (const resources::GpuDevice& gpu : resourcePlan.hardware.gpus)
    {
        RuntimeEvent event;
        event.kind = RuntimeEventKind::ResourceStatus;
        event.state = state.load();
        event.component = gpu.backendId.empty()
            ? "Display GPU " + std::to_string(gpuIndex)
            : gpu.backendId;
        event.phase = "GPU";
        event.resource = gpu.name;
        event.message = gpu.backendId.empty()
            ? "Capacity detected through DXGI; backend identity is unavailable."
            : "Addressable compute device reported by llama.cpp.";
        event.totalMemoryMiB = gpu.totalMemoryMiB;
        event.availableMemoryMiB = gpu.freeMemoryMiB;
        eventBus.Publish(std::move(event));
        ++gpuIndex;
    }

    RuntimeEvent cpu;
    cpu.kind = RuntimeEventKind::ResourceStatus;
    cpu.state = state.load();
    cpu.component = "CPU";
    cpu.phase = "Hardware";
    cpu.resource = std::to_string(resourcePlan.hardware.logicalProcessors) +
        " logical processors";
    cpu.message = std::to_string(settings.resources.reserveLogicalCores) +
        " processors reserved for Windows/UI; chat/background/STT/voice caps are " +
        std::to_string(resourcePlan.chatCpuThreads) + "/" +
        std::to_string(resourcePlan.embeddingCpuThreads) + "/" +
        std::to_string(resourcePlan.speechRecognitionThreads) + "/" +
        std::to_string(resourcePlan.voiceCpuThreads) + ".";
    eventBus.Publish(std::move(cpu));

    RuntimeEvent ram;
    ram.kind = RuntimeEventKind::ResourceStatus;
    ram.state = state.load();
    ram.component = "System RAM";
    ram.phase = "Hardware";
    ram.resource = "Windows mmap + bounded llama cache";
    ram.message = std::to_string(resourcePlan.llamaPromptCacheMiB) +
        " MiB maximum prompt cache plus " +
        std::to_string(resourcePlan.sqliteCacheMiB) +
        " MiB combined SQLite page/mmap ceiling per connection; " +
        std::to_string(resourcePlan.reservedSystemMemoryMiB) +
        " MiB kept free for Windows and other applications.";
    ram.totalMemoryMiB = resourcePlan.hardware.totalSystemMemoryMiB;
    ram.availableMemoryMiB = resourcePlan.hardware.availableSystemMemoryMiB;
    ram.allocatedMemoryMiB = static_cast<std::uint64_t>(
        std::max(0, resourcePlan.llamaPromptCacheMiB + resourcePlan.sqliteCacheMiB));
    eventBus.Publish(std::move(ram));

    const auto assignment = [this](
        const std::string& workload,
        const std::string& resource,
        const std::string& detail)
    {
        RuntimeEvent event;
        event.kind = RuntimeEventKind::ResourceStatus;
        event.state = state.load();
        event.component = workload;
        event.phase = "Assignment";
        event.resource = resource;
        event.message = detail;
        eventBus.Publish(std::move(event));
    };
    assignment(
        "Chat + vision",
        resourcePlan.ChatLabel(),
        resourcePlan.chatSplitMode == "none"
            ? "Latency-first single-device placement."
            : "Model capacity fallback using layer split " +
                resourcePlan.chatTensorSplit + ".");
    assignment(
        "Voice generation",
        resourcePlan.VoiceLabel(),
        resourcePlan.voiceDevices.size() > 1
            ? "Independent Qwen3-TTS workers generate sentence fragments ahead; playback remains ordered."
            : "Long-lived Qwen3-TTS worker generates ahead while playback remains ordered.");
    assignment(
        "Speech recognition",
        resourcePlan.speechRecognitionDevice,
        "Short whisper.cpp bursts use the secondary device when one is available.");
    assignment(
        "Semantic embeddings",
        resourcePlan.embeddingDevice == "none" ? "CPU" : resourcePlan.embeddingDevice,
        "Independent retrieval server; CPU is preferred to protect interactive GPU latency.");
    assignment("Image generation", settings.image.bEnabled ? settings.image.device : "disabled",
        "Lazy image worker; measured free VRAM must cover the greater of the model budget and " +
            std::to_string(settings.image.minimumFreeVramMiB) + " MiB, plus the shared " + std::to_string(settings.image.gpuReserveMiB) +
            " MiB reserve. " +
            (settings.image.bKeepLoaded ? "Weights remain loaded until explicit unload or shutdown." : "Weights release after each job."));
}

void ReviaSession::StartResourceMonitor()
{
    if (settings.resources.usageSampleSeconds <= 0)
    {
        appLogger.Log("Live resource sampling is disabled; the Resources tab will show "
            "the startup plan only.");
        return;
    }
    resourceMonitor.Start(
        resourcePlan,
        std::chrono::seconds(settings.resources.usageSampleSeconds),
        [this](const resources::UsageSnapshot& snapshot)
        {
            PublishResourceUsage(snapshot);
            UpdateResourceLoad(snapshot);
        });
}

void ReviaSession::UpdateResourceLoad(const resources::UsageSnapshot& snapshot)
{
    const resources::LoadAdjustment assessed = resources::AssessLoad(snapshot);
    const auto samePolicy = [](const auto& left, const auto& right)
    {
        return left.state == right.state &&
            left.allowOptionalBackgroundWork == right.allowOptionalBackgroundWork &&
            left.allowOpportunisticVision == right.allowOpportunisticVision;
    };
    bool announce = false;
    bool backgroundRecovered = false;
    {
        std::lock_guard loadLock(loadMutex);
        // Occupancy can stay at 93% while engines become idle. Stabilize admission
        // changes as well as the capacity label, rather than silently overwriting them.
        if (samePolicy(assessed, candidateLoad))
            candidateLoadSamples = std::min(loadSamplesBeforeAdopting, candidateLoadSamples + 1);
        else
        {
            candidateLoad = assessed;
            candidateLoadSamples = 1;
        }
        if (samePolicy(assessed, currentLoad))
            currentLoad = assessed;
        else if (candidateLoadSamples >= loadSamplesBeforeAdopting)
        {
            backgroundRecovered = !currentLoad.allowOptionalBackgroundWork &&
                assessed.allowOptionalBackgroundWork;
            currentLoad = assessed;
            announce = true;
        }
    }
    if (announce)
    {
        PublishComponent("Load", resources::ToString(assessed.state), assessed.reason);
        const std::string detail = "Load " + resources::ToString(assessed.state) + ": " + assessed.reason;
        // Admission control is an expected status, not a runtime fault. Actual model
        // allocation/request failures have their own error events and diagnostics.
        appLogger.Log(detail);
    }
    if (backgroundRecovered && started.load())
    {
        // Re-evaluate existing evidence after a deferral. This signal grants no new
        // evidence, authority, or entitlement to speak; normal attention gates remain.
        SignalInitiative("resources became available for deferred background work");
        SignalCuriosity("resources became available for deferred self-directed review");
    }
}

void ReviaSession::PublishResourceUsage(const resources::UsageSnapshot& snapshot) const
{
    const auto image = imageGenerator.Snapshot();
    RuntimeEvent imageEvent;
    imageEvent.kind = RuntimeEventKind::ResourceStatus;
    imageEvent.state = state.load();
    imageEvent.component = "Image generation";
    imageEvent.phase = image.state;
    imageEvent.resource = image.device.empty() ? "unloaded" : image.device;
    imageEvent.message = image.model + ": " + std::to_string(image.step) + "/" + std::to_string(image.steps) + " steps; weights " +
                         (image.loaded ? "loaded" : "released");
    eventBus.Publish(std::move(imageEvent));
    for (const resources::UsageMeter& meter : snapshot.meters)
    {
        RuntimeEvent event;
        event.kind = RuntimeEventKind::ResourceStatus;
        event.state = state.load();
        event.component = meter.label;
        event.phase = "Usage";
        event.resource = meter.id;
        event.message = meter.detail;
        event.usedAmount = meter.used;
        event.budgetAmount = meter.budget;
        event.capacityAmount = meter.capacity;
        switch (meter.unit)
        {
            case resources::MeterUnit::Threads: event.usageUnit = "threads"; break;
            case resources::MeterUnit::Percent: event.usageUnit = "percent"; break;
            case resources::MeterUnit::Mebibytes: event.usageUnit = "MiB"; break;
        }
        event.usageBasis =
            meter.basis == resources::MeterBasis::Capacity ? "capacity" : "budget";
        event.usageStatus = meter.Status();
        event.usageMeasured = meter.measured;
        eventBus.Publish(std::move(event));
    }
}

resources::UsageSnapshot ReviaSession::ResourceUsage() const
{
    return resourceMonitor.Latest();
}

resources::LoadAdjustment ReviaSession::CurrentLoad() const
{
    std::lock_guard lock(loadMutex);
    return currentLoad;
}

std::string ReviaSession::ResourceUsageStatus() const
{
    if (settings.resources.usageSampleSeconds <= 0)
    {
        return "Live resource sampling is off (resources.usageSampleSeconds is 0).\n\n" +
            resourcePlan.Summary();
    }
    const resources::UsageSnapshot snapshot = resourceMonitor.Latest();
    if (!snapshot.measured)
    {
        return "No live reading has been taken yet.\n\n" + resourcePlan.Summary();
    }
    return snapshot.Detail() + "\n\nPlan: " + resourcePlan.Summary();
}

} // namespace revia::runtime
