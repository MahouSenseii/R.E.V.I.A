#pragma once

#include "Identity/socialIdentity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace revia::identity
{
class RelationshipRegistry;
}

namespace revia::speech
{

inline constexpr std::size_t SpeakerFeatureCount = 16;
inline constexpr std::uint32_t SpeakerFeatureVersion = 1;

struct SpeakerFeatures
{
    std::array<float, SpeakerFeatureCount> values{};
    std::uint32_t version = SpeakerFeatureVersion;
};

struct SpeakerTemplate
{
    std::string entityId;
    std::uint64_t consentRevision = 0;
    SpeakerFeatures features;
};

class ISpeakerRecognitionProvider
{
  public:
    virtual ~ISpeakerRecognitionProvider() = default;
    [[nodiscard]] virtual std::optional<SpeakerFeatures> Extract(
        const std::filesystem::path& transientWave, std::stop_token stopToken, std::string& outError) const = 0;
};

// Bounded local WAV features require no downloaded model; recognition accuracy needs live review.
class WaveSpeakerRecognitionProvider final : public ISpeakerRecognitionProvider
{
  public:
    [[nodiscard]] std::optional<SpeakerFeatures> Extract(
        const std::filesystem::path& transientWave, std::stop_token stopToken, std::string& outError) const override;
};

// Identity owns consent and persisted templates; this collaborator only extracts and matches.
class SpeakerRecognition
{
  public:
    explicit SpeakerRecognition(identity::RelationshipRegistry& identities,
        std::shared_ptr<const ISpeakerRecognitionProvider> provider = std::make_shared<WaveSpeakerRecognitionProvider>());

    bool Enroll(const std::string& entityId, std::uint64_t consentRevision, const SpeakerFeatures& features, std::string& outError);
    bool Enroll(const std::string& entityId, std::uint64_t consentRevision, const std::filesystem::path& transientWave,
        std::string& outError, std::stop_token stopToken = {});
    void Forget(const std::string& entityId);

    [[nodiscard]] identity::SpeakerObservation Resolve(
        const SpeakerFeatures& features, const std::map<std::string, std::uint64_t>& approvedConsentRevisions) const;
    [[nodiscard]] identity::SpeakerObservation Observe(const std::filesystem::path& transientWave,
        const std::map<std::string, std::uint64_t>& approvedConsentRevisions, std::stop_token stopToken = {}) const;

  private:
    identity::RelationshipRegistry& identities;
    std::shared_ptr<const ISpeakerRecognitionProvider> provider;
};

} // namespace revia::speech
