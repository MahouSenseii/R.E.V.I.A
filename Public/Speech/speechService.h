#pragma once

#include "Library/structLibrary.h"
#include "Runtime/affectTypes.h"
#include "Speech/qwenTtsPool.h"
#include "Speech/orderedSpeechQueue.h"
#include "Speech/voiceActivityMonitor.h"
#include "Speech/voicePresetStore.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace revia::speech
{

struct SpeechEvent
{
    SpeechEvent() = default;
    SpeechEvent(
        std::string inputPhase,
        std::string inputDetail,
        const double inputElapsedMilliseconds = -1.0,
        const int inputQueueDepth = 0,
        const std::uint64_t inputUtteranceId = 0,
        std::string inputDevice = {})
        : phase(std::move(inputPhase)),
          detail(std::move(inputDetail)),
          elapsedMilliseconds(inputElapsedMilliseconds),
          queueDepth(inputQueueDepth),
          utteranceId(inputUtteranceId),
          device(std::move(inputDevice))
    {
    }

    std::string phase;
    std::string detail;
    double elapsedMilliseconds = -1.0;
    int queueDepth = 0;
    // Which reply this event belongs to, so the shell can hold that reply's text until
    // the audio for it actually starts. Zero when the event is not about an utterance.
    std::uint64_t utteranceId = 0;
    std::string device;
    std::vector<latencySample> timings;
};

class SpeechService
{
public:
    using EventHandler = std::function<void(const SpeechEvent&)>;

    SpeechService() = default;
    ~SpeechService();

    SpeechService(const SpeechService&) = delete;
    SpeechService& operator=(const SpeechService&) = delete;

    bool Start(const speechSettings& settings, EventHandler handler);
    void SetActiveProfile(std::string profileId);
    VoiceStudioSnapshot VoiceStudio() const;
    bool HasActiveQwenVoice() const;
    VoiceOperationResult PrepareActiveVoice();
    VoiceOperationResult CreateVoicePreset(
        const std::string& name,
        const std::string& description,
        const std::string& referenceText,
        const std::string& language);
    VoiceOperationResult PreviewVoice(const std::string& presetId, const std::string& text);
    VoiceOperationResult AssignVoice(const std::string& profileId, const std::string& presetId);
    void SetEnabled(bool enabled);
    bool IsEnabled() const;
    [[nodiscard]] bool HasPendingSpeech() const;
    [[nodiscard]] std::size_t FirstFragmentCharacters() const;
    [[nodiscard]] std::size_t PreferredFragmentCharacters() const;
    // utteranceId correlates the resulting Speaking event back to the reply, so the shell
    // can reveal text in step with the audio instead of well before it.
    void Speak(
        std::string text,
        runtime::AffectSnapshot affect,
        std::uint64_t utteranceId = 0,
        bool latencyCritical = true);
    void StopSpeaking();

    // Barge-in. Arms a microphone energy monitor only for the duration of each utterance,
    // and yields the floor when the user starts talking over Revia.
    void ConfigureBargeIn(const bargeInSettings& settings, int sampleRate);
    void SetBargeInHandler(std::function<void()> handler);
    void SetBargeInEnabled(bool enabled);
    [[nodiscard]] bool IsBargeInEnabled() const;
    // Stops talking without cancelling the Qwen request that produced the audio. Killing
    // that worker would make the next reply pay a full model reload, which is far too
    // expensive a price for the user having spoken.
    void YieldToUser();

    // Shutdown-only escape hatch for a model load or synthesis blocked inside the
    // Python worker. Normal Stop/Yield deliberately keep the warm workers alive.
    void CancelVoiceOperationsForShutdown();

    void Shutdown();

    // keepVocalizations is true only for a backend that performs an inline nonverbal
    // cue itself. Windows SAPI cannot, and for it the tag is dropped rather than
    // flattened, because reading the word "laughs" aloud is worse than silence.
    static std::string NormalizeForSpeech(
        const std::string& text,
        std::size_t maxCharacters,
        bool keepVocalizations = false);

private:
    struct Utterance
    {
        std::string text;
        runtime::AffectSnapshot affect;
        std::uint64_t generation = 0;
        std::uint64_t utteranceId = 0;
        std::uint64_t sequence = 0;
        bool latencyCritical = true;
        std::chrono::steady_clock::time_point queuedAt =
            std::chrono::steady_clock::now();
        std::optional<VoicePreset> preset;
    };

    struct PreparedUtterance
    {
        Utterance utterance;
        std::filesystem::path audioPath;
        VoiceOperationResult result;
        bool qwenAttempted = false;
        std::uintmax_t bufferedBytes = 0;
    };

    void Run(std::stop_token stopToken);
    void Generate(std::stop_token stopToken);
    // Synthesizes one phrase on its own and publishes it. Also the fallback body for a
    // batch that could not run, so the two paths cannot drift apart in what they
    // publish or in how they account for a phrase still being generated.
    void SynthesizeOne(Utterance utterance, int depth);
    // Attempts one generation call covering the whole group, in order.
    //
    // False means nothing was published and the caller must fall back to synthesizing
    // the group one phrase at a time. Every refusal is reported that way rather than by
    // throwing: a batch that will not fit is an ordinary condition on a shared card,
    // not an error, and the per-phrase path is always available.
    bool SynthesizeBatch(std::vector<Utterance>& group, int depth);
    // Takes the phrases that may ride along with `leader` off the queue.
    //
    // Caller holds the mutex. Only complete phrases already waiting are taken: nothing
    // is held back to build a bigger batch, because the phrases are still arriving from
    // the model and waiting for more would delay the ones already here.
    std::vector<Utterance> CollectBatchCompanions(const Utterance& leader);
    void PublishGenerated(const Utterance& utterance, PreparedUtterance item, int depth);
    // Compares the path the first real phrase ran on against the configured one, once
    // per session. Graph capture is deferred to that phrase, so this is the earliest
    // point at which the answer is an observation rather than a setting.
    void VerifyInferenceBackend(
        const VoiceOperationResult& result, std::uint64_t utteranceId);
    bool PlayPreparedQwen(const PreparedUtterance& prepared);
    void Notify(SpeechEvent event) const;
    void ArmBargeIn();
    void DisarmBargeIn();

    mutable std::mutex mutex;
    std::condition_variable_any condition;
    std::deque<Utterance> queue;
    OrderedSpeechQueue playbackOrder;
    std::map<std::uint64_t, PreparedUtterance> prepared;
    std::size_t generatingCount = 0;
    std::uintmax_t bufferedAudioBytes = 0;
    speechSettings configuration;
    std::string activeProfile;
    std::optional<VoicePreset> activePreset;
    EventHandler eventHandler;
    VoicePresetStore presetStore;
    QwenTtsPool qwenPool;
    VoiceActivityMonitor bargeInMonitor;
    std::function<void()> bargeInHandler;
    std::jthread worker;
    std::vector<std::jthread> generationWorkers;
    std::atomic<bool> enabled = false;
    std::atomic<bool> ready = false;
    std::atomic<std::uint64_t> generation = 1;
    std::atomic<std::uint64_t> nextSequence = 1;
    // Stamped onto every event while an utterance is in flight, so callers do not have to
    // thread the id through each Notify call site.
    std::atomic<std::uint64_t> activeUtteranceId = 0;
    // Latched by the first synthesised phrase, so the backend check is reported once
    // rather than on every request.
    std::atomic<bool> backendVerified = false;
};

} // namespace revia::speech
