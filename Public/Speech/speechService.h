#pragma once

#include "Library/structLibrary.h"
#include "Runtime/affectTypes.h"
#include "Speech/qwenTtsClient.h"
#include "Speech/voiceActivityMonitor.h"
#include "Speech/voicePresetStore.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

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
    // utteranceId correlates the resulting Speaking event back to the reply, so the shell
    // can reveal text in step with the audio instead of well before it.
    void Speak(std::string text, runtime::AffectSnapshot affect, std::uint64_t utteranceId = 0);
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

    void Shutdown();

    static std::string NormalizeForSpeech(const std::string& text, std::size_t maxCharacters);

private:
    struct Utterance
    {
        std::string text;
        runtime::AffectSnapshot affect;
        std::uint64_t generation = 0;
        std::uint64_t utteranceId = 0;
    };

    void Run(std::stop_token stopToken);
    bool SpeakWithQwen(const Utterance& utterance, const VoicePreset& preset);
    void Notify(SpeechEvent event) const;
    void ArmBargeIn();
    void DisarmBargeIn();

    mutable std::mutex mutex;
    std::condition_variable_any condition;
    std::deque<Utterance> queue;
    speechSettings configuration;
    std::string activeProfile;
    std::optional<VoicePreset> activePreset;
    EventHandler eventHandler;
    VoicePresetStore presetStore;
    QwenTtsClient qwenClient;
    VoiceActivityMonitor bargeInMonitor;
    std::function<void()> bargeInHandler;
    std::jthread worker;
    std::atomic<bool> enabled = false;
    std::atomic<bool> ready = false;
    std::atomic<std::uint64_t> generation = 1;
    // Stamped onto every event while an utterance is in flight, so callers do not have to
    // thread the id through each Notify call site.
    std::atomic<std::uint64_t> activeUtteranceId = 0;
};

} // namespace revia::speech
