#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::identity
{

// Whose voice this is, when they asked her to learn it.
//
// A voiceprint is a few unit embeddings of one person's utterances, kept only because
// that person asked ("Revia, remember my voice, I'm Sam" or /voice enroll Sam) and
// dropped the moment they ask (/voice forget). A match is a signal about who is
// probably at the microphone -- it moves conversation attribution exactly as a stated
// name does and grants nothing else, because a cloned voice defeats a voiceprint. The
// numbers stay on this machine: the session keeps them in the DPAPI secret store and
// they never enter a prompt, a log or a memory.
struct Voiceprint
{
    std::string entityId;
    std::string displayName;
    // Unit vectors, newest last, at most `maximumSamples`.
    std::vector<std::vector<float>> samples;
    // When the person asked, as an ISO date-time, for the record.
    std::string consentedAt;
};

struct SpeakerMatch
{
    bool matched = false;
    std::string entityId;
    std::string displayName;
    // Best cosine similarity, and how far the runner-up was behind.
    float score = 0.0F;
    float margin = 0.0F;
    // The nearest print even when it did not match, for the log.
    std::string nearest;
    // Why it was or was not a match, in a few words.
    std::string reason;
};

class VoiceprintRegistry
{
public:
    explicit VoiceprintRegistry(float threshold = 0.62F, float margin = 0.08F, std::size_t maximumSamples = 5);

    void Configure(float threshold, float margin);
    // Adds one sample to the person's print, making it when there is none. Rolling:
    // the oldest sample goes once there are `maximumSamples`. Returns how many samples
    // the print holds now, or 0 with `outError`.
    std::size_t Enroll(
        const std::string& entityId,
        const std::string& displayName,
        const std::vector<float>& embedding,
        const std::string& consentedAt,
        std::string& outError);
    bool Forget(const std::string& entityId);
    void Clear();

    [[nodiscard]] SpeakerMatch Match(const std::vector<float>& embedding) const;
    [[nodiscard]] std::vector<Voiceprint> All() const;
    [[nodiscard]] std::size_t Count() const;
    [[nodiscard]] float Threshold() const { return threshold; }

    // The whole registry as JSON, and back. What the secret store holds.
    [[nodiscard]] std::string Serialize() const;
    bool Deserialize(const std::string& json, std::string& outError);

    [[nodiscard]] std::string Describe() const;
    [[nodiscard]] static float Cosine(const std::vector<float>& a, const std::vector<float>& b);
    [[nodiscard]] static std::vector<float> Normalized(const std::vector<float>& embedding);

private:
    mutable std::mutex mutex;
    float threshold;
    float margin;
    std::size_t maximumSamples;
    std::vector<Voiceprint> prints;
};

// Whether the words ask her to learn the speaker's voice: "remember my voice",
// "learn my voice", "enroll my voice". Consent is the request itself.
[[nodiscard]] bool AsksToEnrollVoice(const std::string& input);
// Whether they ask her to forget it.
[[nodiscard]] bool AsksToForgetVoice(const std::string& input);
// The name in a request to keep a voice: a stated name ("my name is Sam", "call me
// Sam"), or, only here, "I'm Sam" / "it's Sam" / "this is Sam" with the name
// capitalised as a transcript writes one -- "I'm tired" names nobody. Empty when
// no name was said.
[[nodiscard]] std::string ReadVoiceEnrollmentName(const std::string& input);

} // namespace revia::identity
