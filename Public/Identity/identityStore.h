#pragma once

#include "Emotion/moodState.h"
#include "Identity/developmentState.h"
#include "Identity/preferenceState.h"
#include "Identity/relationshipState.h"
#include "Identity/socialIdentity.h"
#include "Speech/speakerRecognition.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::identity
{

// Persistent mood, development and relationships; momentary emotion is excluded.
struct IdentitySnapshot
{
    DevelopmentState development;
    emotion::MoodState mood;
    std::map<std::string, RelationshipState> relationships;
    std::vector<DevelopmentChange> developmentHistory;
    std::vector<Preference> preferences;
    std::map<std::string, RelationshipState> relationshipEvidenceBase;
    std::vector<RelationshipEvidenceRecord> relationshipEvidence;
    std::vector<AudienceAlias> audienceAliases;
    std::map<std::string, RecognitionConsent> recognitionConsents;
    std::map<std::string, speech::SpeakerTemplate> speakerTemplates;
};

// 3 adds private aliases, consented voice templates and retained correctable social evidence.
// Older documents load without inventing consent; older builds refuse this newer schema.
inline constexpr int IdentitySchemaVersion = 3;

// Atomic, versioned identity JSON keyed by names; enum reordering cannot reinterpret saved state.
class IdentityStore
{
  public:
    explicit IdentityStore(std::filesystem::path path = "RuntimeData/Identity/identity.json");

    // A missing file is a first run, not a failure: it yields the childlike baseline
    // and reports success, because refusing to start without a prior life would be
    // absurd. A file that exists but cannot be parsed IS a failure, and is reported as
    // one rather than being silently replaced with a fresh personality.
    [[nodiscard]] bool Load(IdentitySnapshot& outSnapshot, std::string& outError) const;
    [[nodiscard]] bool Save(const IdentitySnapshot& snapshot, std::string& outError) const;

    [[nodiscard]] std::filesystem::path Path() const
    {
        return storePath;
    }

    // Version of the file currently on disk, or nullopt when there is none. Exposed so a
    // migration can be decided without a full load.
    [[nodiscard]] std::optional<int> StoredVersion() const;

  private:
    std::filesystem::path storePath;
    mutable std::mutex mutex;
};

} // namespace revia::identity
