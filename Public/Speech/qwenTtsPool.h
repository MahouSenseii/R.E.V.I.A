#pragma once

#include "Library/structLibrary.h"
#include "Speech/qwenTtsClient.h"
#include "Speech/voiceTypes.h"

#include <condition_variable>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace revia::speech
{

// Data-parallel Qwen workers. Each process owns one complete model on one device; a
// heterogeneous pair is useful for independent bounded phrases, not for splitting one
// autoregressive synthesis request across PCIe devices.
class QwenTtsPool
{
public:
    QwenTtsPool() = default;
    ~QwenTtsPool();

    QwenTtsPool(const QwenTtsPool&) = delete;
    QwenTtsPool& operator=(const QwenTtsPool&) = delete;

    void Configure(const speechSettings& settings);
    [[nodiscard]] std::size_t WorkerCount() const;
    VoiceOperationResult PrepareVoice(const VoicePreset& preset);
    VoiceOperationResult DesignVoice(
        const std::string& text,
        const std::string& description,
        const std::string& language,
        const std::string& outputPath);
    VoiceOperationResult Synthesize(
        const std::string& text,
        const VoicePreset& preset,
        const std::string& outputPath,
        bool latencyCritical = false);
    VoiceOperationResult SynthesizePcm(
        const std::string& text,
        const VoicePreset& preset,
        bool latencyCritical = false);
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
        double fixedOverheadMilliseconds = 5000.0;
        double millisecondsPerCharacter = 180.0;
        double firstPhraseMilliseconds = -1.0;
        std::chrono::steady_clock::time_point predictedCompletion{};
        std::uint64_t completed = 0;
    };

    std::size_t AcquireWorker(std::size_t characters, bool latencyCritical);
    void ReleaseWorker(std::size_t index, std::size_t characters, double milliseconds);

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::vector<Worker> workers;
    std::mutex designMutex;
    std::unique_ptr<QwenTtsClient> designClient;
    speechSettings designSettings;
    bool shuttingDown = false;
};

} // namespace revia::speech
