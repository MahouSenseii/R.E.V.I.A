#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace revia::identity
{

enum class AudienceKind
{
    Unknown,
    Private,
    Shared,
    Public
};

// The host captures disclosure context; speaker similarity never creates a private audience.
struct AudienceContext
{
    AudienceKind kind = AudienceKind::Unknown;
    std::string audienceId;
    std::uint64_t revision = 0;
    std::vector<std::string> recipientEntityIds;
};

enum class SpeakerSource
{
    Unknown,
    ExplicitIntroduction,
    Platform,
    ConsentedVoice
};

// Similarity is matching evidence, not a probability or an authorization decision.
struct SpeakerObservation
{
    std::string entityId;
    SpeakerSource source = SpeakerSource::Unknown;
    std::string observationId;
    std::uint64_t consentRevision = 0;
    float similarity = 0.0F;
};

struct RecognitionConsent
{
    bool granted = false;
    std::uint64_t revision = 0;
};

struct AudienceAlias
{
    std::string entityId;
    std::string audienceId;
    std::string recipientEntityId;
    std::string alias;
};

[[nodiscard]] std::string ToString(AudienceKind audience);
[[nodiscard]] std::string ToString(SpeakerSource source);

} // namespace revia::identity
