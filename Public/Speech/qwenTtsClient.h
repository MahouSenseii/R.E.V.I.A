#pragma once

#include "Speech/speechSettings.h"
#include "Speech/qwenTtsServerProcess.h"
#include "Speech/vocalization.h"
#include "Speech/voiceTypes.h"

#include <filesystem>
#include <mutex>
#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace revia::speech
{

// Accepts batch clip lengths only when well formed, matching clip count and every payload byte.
// Any disagreement aborts the batch; positional phrase/audio mappings are never repaired.
[[nodiscard]] std::optional<std::vector<std::size_t>> ParseBatchClipSizes(const std::string& header,
    std::size_t payloadBytes, std::size_t expectedClips);

class QwenTtsClient
{
public:
    QwenTtsClient() = default;
    ~QwenTtsClient();

    QwenTtsClient(const QwenTtsClient&) = delete;
    QwenTtsClient& operator=(const QwenTtsClient&) = delete;

    void Configure(speechSettings settings);
    bool IsAvailable(std::string& outDetail);
    VoiceOperationResult PrepareVoice(const VoicePreset& preset);
    VoiceOperationResult DesignVoice(const std::string& text,
        const std::string& description, const std::string& language, const std::string& outputPath);
    // How many clips a kind gets. More than one so a repeated laugh is not the same
    // recording twice; few enough that rendering a bank stays a one-off cost.
    struct VocalizationRequest
    {
        VocalizationKind kind = VocalizationKind::Laugh;
        int variants = 2;
    };
    // VoiceDesign renders <presetDirectory>/vocalizations/<kind>-<n>.wav, numbered from 1.
    // existingOnly generates kinds without clips, keeping repeated preparation idempotent.
    VoiceOperationResult RenderVocalizations(const std::filesystem::path& presetDirectory, const std::vector<VocalizationRequest>& kinds,
        const std::string& language = "English", bool missingOnly = true);

    // What a complete bank means, in one place, so generation and any status display
    // cannot disagree about whether a voice is finished. Laughter gets the most
    // variants because it is the cue she reaches for most, and a repeated identical
    // laugh is the one that gives the trick away.
    [[nodiscard]] static std::vector<VocalizationRequest> DefaultVocalizationBankRequests();
    VoiceOperationResult Synthesize(const std::string& text, const VoicePreset& preset, const std::string& outputPath);
    VoiceOperationResult SynthesizePcm(const std::string& text, const VoicePreset& preset);
    // Synthesizes complete phrases into ordered results, one per input, or one failed result.
    // Never returns a short/reordered success vector that could mismatch playback slots.
    std::vector<VoiceOperationResult> SynthesizePcmBatch(const std::vector<std::string>& texts, const VoicePreset& preset);
    void CancelActiveRequest();
    void Shutdown();

private:
    bool EnsureAvailable(std::string& outError);
    VoiceOperationResult Post(const std::string& endpoint, const std::string& body);
    VoiceOperationResult PostAudio(const std::string& endpoint, const std::string& body);

    std::mutex mutex;
    std::mutex processMutex;
    // Set by Shutdown and cleared only by Configure. Nothing may start the worker again
    // while it is set: a synthesis failing because shutdown killed its process used to
    // come straight back through EnsureAvailable and reload the model -- about 20 s of a
    // /quit spent loading a voice in order to close it.
    std::atomic<bool> closed = false;
    // Set only for the moment CancelActiveRequest is killing the process, so a request
    // racing the kill does not restart it. Separate from closed so that clearing it can
    // never reopen a client that is shutting down.
    std::atomic<bool> cancelling = false;
    speechSettings configuration;
    std::string apiKey;
    QwenTtsServerProcess process;
};

} // namespace revia::speech
