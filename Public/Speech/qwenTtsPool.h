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

// One worker's scheduling state, without the client or the device behind it.
//
// Split out so the choice can be tested directly. The bug this shape exists to prevent
// is a property of the comparison rather than of the transport, and it is not
// reachable through a pool that needs two Python processes to exist first.
struct VoiceWorkerState
{
    bool busy = false;
    double fixedOverheadMilliseconds = 5000.0;
    double millisecondsPerCharacter = 180.0;
};

// The worker to dispatch to, or workers.size() when every eligible worker is busy and
// the caller has to wait.
//
// Only idle workers are candidates. A busy worker is not a candidate however good its
// measured rate is, and that is the whole correction: the previous version compared
// busy and idle workers on predicted finish time and then waited when the winner was
// busy. That is defensible for one request in isolation and wrong for a queue, because
// every waiting caller runs the same comparison and reaches the same answer -- so a
// long reply could park several phrases on one card while the other sat idle. The live
// 2026-09-02 session split 49 phrases to the RTX 2070 against 17 to the RTX 5070 with
// queue depth reaching 8.
//
// Predicted duration still decides between idle workers, where it costs nothing to be
// wrong: the alternative worker is free either way.
[[nodiscard]] std::size_t SelectIdleVoiceWorker(
    const std::vector<VoiceWorkerState>& workers,
    std::size_t characters,
    bool latencyCritical);

struct VoicePoolTestAccess;

// Data-parallel Qwen workers. Each process owns one complete model on one device; a
// heterogeneous pair is useful for independent bounded phrases, not for splitting one
// autoregressive synthesis request across PCIe devices.
class QwenTtsPool
{
    // The wait-and-recompute loop is the part that regressed, and it is not reachable
    // through the synthesis entry points without a live Python worker on every device.
    // The tests reach the scheduler directly rather than standing up two model servers
    // to observe a comparison that never needed one.
    friend struct VoicePoolTestAccess;

public:
    QwenTtsPool() = default;
    ~QwenTtsPool();

    QwenTtsPool(const QwenTtsPool&) = delete;
    QwenTtsPool& operator=(const QwenTtsPool&) = delete;

    void Configure(const speechSettings& settings);
    [[nodiscard]] std::size_t WorkerCount() const;
    VoiceOperationResult PrepareVoice(const VoicePreset& preset);
    // Renders one preset's nonverbal clip bank. Routed through the same isolated,
    // on-demand VoiceDesign worker as DesignVoice, because that is the only model that
    // accepts the style instruction which produces a sound instead of the word.
    VoiceOperationResult RenderVocalizations(
        const std::filesystem::path& presetDirectory,
        const std::vector<QwenTtsClient::VocalizationRequest>& kinds,
        const std::string& language,
        bool missingOnly);
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
    // One generation call covering several complete phrases, on a single worker.
    //
    // Never latency-critical by construction: the first phrase of a reply is always
    // synthesized on its own so it can start playing, and only the phrases behind it
    // are batched. Returns one result per text in order, or a single failed result the
    // caller falls back from.
    std::vector<VoiceOperationResult> SynthesizePcmBatch(
        const std::vector<std::string>& texts,
        const VoicePreset& preset);
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
        std::uint64_t completed = 0;
    };

    // Blocks until a worker is held. outWaitMilliseconds is filled with how long that
    // took, including the case where the pool shut down while waiting -- a request
    // that waited and then got nothing still waited, and hiding that would make the
    // shutdown path look instant.
    std::size_t AcquireWorker(
        std::size_t characters,
        bool latencyCritical,
        double& outWaitMilliseconds);
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
