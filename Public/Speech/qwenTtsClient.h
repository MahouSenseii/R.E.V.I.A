#pragma once

#include "Library/structLibrary.h"
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

// Splits the clip-length header of a batched synthesis reply.
//
// Returns nothing unless the header is well formed, names exactly the expected number
// of clips, and accounts for every byte of the payload. The mapping from clip to phrase
// is positional, so a split that is merely plausible is worse than no split at all: it
// would attach one phrase's audio to another phrase's text and play it in a reply that
// otherwise looks correct. Every disagreement therefore aborts the batch rather than
// being repaired.
[[nodiscard]] std::optional<std::vector<std::size_t>> ParseBatchClipSizes(
    const std::string& header,
    std::size_t payloadBytes,
    std::size_t expectedClips);

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
    VoiceOperationResult DesignVoice(
        const std::string& text,
        const std::string& description,
        const std::string& language,
        const std::string& outputPath);
    // How many clips a kind gets. More than one so a repeated laugh is not the same
    // recording twice; few enough that rendering a bank stays a one-off cost.
    struct VocalizationRequest
    {
        VocalizationKind kind = VocalizationKind::Laugh;
        int variants = 2;
    };
    // Renders the nonverbal clip bank for one voice preset into
    // <presetDirectory>/vocalizations, as <kind>-<n>.wav starting at 1 -- the exact
    // layout VocalizationBank scans for.
    //
    // A batch job at preset-creation time rather than a runtime call, because a laugh
    // that arrives a second after the joke is not a laugh. Only the VoiceDesign model
    // accepts the style instruction that produces a sound instead of the word, so this
    // is the one path that can render them in Revia's own voice.
    //
    // `existingOnly` renders just the kinds that have no clip yet, which is what makes
    // repeated preparation cheap and idempotent.
    VoiceOperationResult RenderVocalizations(
        const std::filesystem::path& presetDirectory,
        const std::vector<VocalizationRequest>& kinds,
        const std::string& language = "English",
        bool missingOnly = true);

    // What a complete bank means, in one place, so generation and any status display
    // cannot disagree about whether a voice is finished. Laughter gets the most
    // variants because it is the cue she reaches for most, and a repeated identical
    // laugh is the one that gives the trick away.
    [[nodiscard]] static std::vector<VocalizationRequest>
        DefaultVocalizationBankRequests();
    VoiceOperationResult Synthesize(
        const std::string& text,
        const VoicePreset& preset,
        const std::string& outputPath);
    VoiceOperationResult SynthesizePcm(
        const std::string& text,
        const VoicePreset& preset);
    // Synthesizes several complete phrases in one generation call.
    //
    // Returns one result per input text, in the order given, or a single failed result
    // whose message says why the batch could not run. A short or reordered vector is
    // never returned: the caller maps these onto playback slots by position, so a
    // mismatch has to surface as a failure it can fall back from rather than as audio
    // attached to the wrong phrase.
    std::vector<VoiceOperationResult> SynthesizePcmBatch(
        const std::vector<std::string>& texts,
        const VoicePreset& preset);
    void CancelActiveRequest();
    void Shutdown();

private:
    bool EnsureAvailable(std::string& outError);
    VoiceOperationResult Post(const std::string& endpoint, const std::string& body);
    VoiceOperationResult PostAudio(const std::string& endpoint, const std::string& body);

    std::mutex mutex;
    std::mutex processMutex;
    std::atomic<bool> shuttingDown = false;
    speechSettings configuration;
    std::string apiKey;
    QwenTtsServerProcess process;
};

} // namespace revia::speech
