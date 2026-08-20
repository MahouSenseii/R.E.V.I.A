#pragma once

#include "Library/structLibrary.h"

#include <string>
#include <cstdint>

struct llamaHardwarePlan
{
    int contextTokens = 4096;
    int parallelRequests = 1;
};

struct llamaHardwareMemory
{
    std::uint64_t dedicatedVideoMemoryMiB = 0;
    std::uint64_t systemMemoryMiB = 0;
};

[[nodiscard]] llamaHardwareMemory DetectLlamaHardwareMemory();

// Pure hardware policy used by automatic startup and tests. Parallel slots are granted
// conservatively after subtracting VRAM reserved for another local model such as Qwen TTS.
[[nodiscard]] llamaHardwarePlan PlanLlamaHardware(
    std::uint64_t dedicatedVideoMemoryMiB,
    std::uint64_t systemMemoryMiB,
    int reservedVramMiB);

class llamaCppServerProcess
{
public:
    llamaCppServerProcess() = default;
    ~llamaCppServerProcess();

    llamaCppServerProcess(const llamaCppServerProcess&) = delete;
    llamaCppServerProcess& operator=(const llamaCppServerProcess&) = delete;

    bool Start(const llmSettings& settings, std::string& outError);
    bool StartEmbedding(const embeddingSettings& settings, std::string& outError);
    bool IsRunning() const;
    bool WasStartedByRevia() const;
    void Stop();

private:
    bool StartInternal(
        const llmSettings& settings,
        bool embeddingMode,
        const std::string& pooling,
        const std::string& device,
        std::string& outError);
#ifdef _WIN32
    void* processHandle = nullptr;
    void* jobHandle = nullptr;
#endif
    bool bShutdownOnExit = true;
};
