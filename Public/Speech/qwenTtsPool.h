#pragma once

#include "Library/structLibrary.h"
#include "Speech/qwenTtsClient.h"
#include "Speech/voiceTypes.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace revia::speech
{

// Data-parallel Qwen workers. Each process owns one complete model on one device; a
// heterogeneous pair is useful for independent sentences, not for splitting one
// autoregressive sentence across PCIe devices.
class QwenTtsPool
{
public:
    QwenTtsPool() = default;
    ~QwenTtsPool();

    QwenTtsPool(const QwenTtsPool&) = delete;
    QwenTtsPool& operator=(const QwenTtsPool&) = delete;

    void Configure(const speechSettings& settings);
    [[nodiscard]] std::size_t WorkerCount() const;
    VoiceOperationResult PrepareCloneModel();
    VoiceOperationResult DesignVoice(
        const std::string& text,
        const std::string& description,
        const std::string& language,
        const std::string& outputPath);
    VoiceOperationResult Synthesize(
        const std::string& text,
        const VoicePreset& preset,
        const std::string& outputPath);
    void CancelActiveRequests();
    // Prevents waiting generators from acquiring another worker and interrupts requests
    // already inside Python. Worker objects remain valid until Shutdown after joins.
    void RequestShutdown();
    void Shutdown();

private:
    struct Worker
    {
        std::string id;
        std::string device;
        std::unique_ptr<QwenTtsClient> client;
        bool busy = false;
        double millisecondsPerCharacter = 35.0;
        std::uint64_t completed = 0;
    };

    std::size_t AcquireWorker(std::size_t characters);
    void ReleaseWorker(std::size_t index, std::size_t characters, double milliseconds);

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::vector<Worker> workers;
    bool shuttingDown = false;
};

} // namespace revia::speech
