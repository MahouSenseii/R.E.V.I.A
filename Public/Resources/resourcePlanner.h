#pragma once

#include "Library/structLibrary.h"

#include <cstdint>
#include <string>
#include <vector>

namespace revia::resources
{

struct GpuDevice
{
    std::string backendId;
    std::string name;
    int ordinal = -1;
    std::uint64_t totalMemoryMiB = 0;
    std::uint64_t freeMemoryMiB = 0;

    [[nodiscard]] bool IsCuda() const;
    [[nodiscard]] std::string QwenDevice() const;
};

struct HardwareInventory
{
    std::vector<GpuDevice> gpus;
    std::uint64_t totalSystemMemoryMiB = 0;
    std::uint64_t availableSystemMemoryMiB = 0;
    unsigned int logicalProcessors = 1;
    bool exactBackendDevices = false;
    std::string detail;
};

struct ResourceRequirements
{
    std::uint64_t chatWorkingSetMiB = 0;
    bool voiceExpected = false;
    bool speechRecognitionGpuEnabled = true;
    int voiceMinimumVramMiB = 4600;
    int baseGpuReserveMiB = 1536;
};

struct ResourcePlan
{
    HardwareInventory hardware;
    bool automatic = true;
    std::vector<GpuDevice> chatGpus;
    std::string chatDevice = "auto";
    std::string chatSplitMode = "none";
    std::string chatTensorSplit;
    std::string chatFitTargets;
    std::string voiceDevice = "cpu";
    std::string embeddingDevice = "none";
    std::string speechRecognitionDevice = "cpu";
    int chatCpuThreads = 0;
    int chatBatchThreads = 0;
    int embeddingCpuThreads = 0;
    int speechRecognitionThreads = 0;
    int voiceCpuThreads = 0;
    int llamaPromptCacheMiB = 0;
    int sqliteCacheMiB = 0;
    int reservedSystemMemoryMiB = 0;
    std::vector<std::string> notes;

    [[nodiscard]] std::string ChatLabel() const;
    [[nodiscard]] std::string VoiceLabel() const;
    [[nodiscard]] std::string Summary() const;
};

// Uses the exact backend executable rather than guessing CUDA ordinals from display
// adapters. A DXGI fallback still reports capacity when --list-devices is unavailable,
// but the planner will leave backend selection on auto rather than emit an invalid ID.
[[nodiscard]] HardwareInventory DetectHardwareInventory(
    const std::string& llamaServerExecutable);

[[nodiscard]] ResourceRequirements EstimateResourceRequirements(
    const appSettings& settings,
    bool voiceExpected);

// Pure policy entry point used with synthetic inventories in tests.
[[nodiscard]] ResourcePlan PlanResources(
    const HardwareInventory& hardware,
    const resourceSettings& policy,
    const ResourceRequirements& requirements);

// Applies the immutable plan to service-specific launch settings. It starts no process.
void ApplyResourcePlan(const ResourcePlan& plan, appSettings& settings);

} // namespace revia::resources
