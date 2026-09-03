#pragma once

#include "Library/structLibrary.h"
#include "Speech/qwenTtsServerProcess.h"
#include "Speech/voiceTypes.h"

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
