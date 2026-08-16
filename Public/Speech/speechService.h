#pragma once

#include "Library/structLibrary.h"
#include "Runtime/affectTypes.h"
#include "Speech/qwenTtsClient.h"
#include "Speech/voicePresetStore.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace revia::speech
{

struct SpeechEvent
{
    std::string phase;
    std::string detail;
    double elapsedMilliseconds = -1.0;
    int queueDepth = 0;
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
    void Speak(std::string text, runtime::AffectSnapshot affect);
    void StopSpeaking();
    void Shutdown();

    static std::string NormalizeForSpeech(const std::string& text, std::size_t maxCharacters);

private:
    struct Utterance
    {
        std::string text;
        runtime::AffectSnapshot affect;
        std::uint64_t generation = 0;
    };

    void Run(std::stop_token stopToken);
    bool SpeakWithQwen(const Utterance& utterance, const VoicePreset& preset);
    void Notify(SpeechEvent event) const;

    mutable std::mutex mutex;
    std::condition_variable_any condition;
    std::deque<Utterance> queue;
    speechSettings configuration;
    std::string activeProfile;
    std::optional<VoicePreset> activePreset;
    EventHandler eventHandler;
    VoicePresetStore presetStore;
    QwenTtsClient qwenClient;
    std::jthread worker;
    std::atomic<bool> enabled = false;
    std::atomic<bool> ready = false;
    std::atomic<std::uint64_t> generation = 1;
};

} // namespace revia::speech
