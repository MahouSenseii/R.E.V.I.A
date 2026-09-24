#pragma once

#include "Emotion/moodState.h"
#include "Identity/developmentState.h"
#include "Identity/preferenceState.h"
#include "Identity/relationshipState.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::identity
{

// The parts of Revia that must survive a restart.
//
// Deliberately not everything. A momentary emotion is not saved, because restoring one
// would mean she resumes being annoyed about something she can no longer remember. Mood
// is saved, because a bad afternoon reasonably outlasts a process, and development and
// relationships are saved because losing them would make every launch a new person with
// a familiar voice.
struct IdentitySnapshot
{
    DevelopmentState development;
    emotion::MoodState mood;
    std::map<std::string, RelationshipState> relationships;
    std::vector<DevelopmentChange> developmentHistory;
    std::vector<Preference> preferences;
};

// 2 added preferences. Bumped rather than added silently: an older build refuses a file
// from a newer schema instead of loading it and dropping the keys it does not know, and
// dropping them would delete opinions on the next save.
inline constexpr int IdentitySchemaVersion = 2;

// Loads and saves identity as one versioned JSON document.
//
// Atomic on write: a half-written identity file would be worse than an absent one,
// because the absent one is obviously a first run while the truncated one silently
// resets whichever traits happened to be past the cut.
//
// Everything is keyed by name rather than by position, so reordering an enum or adding
// a trait cannot reinterpret an existing file as a different personality.
class IdentityStore
{
public:
    explicit IdentityStore(
        std::filesystem::path path = "RuntimeData/Identity/identity.json");

    // A missing file is a first run, not a failure: it yields the childlike baseline
    // and reports success, because refusing to start without a prior life would be
    // absurd. A file that exists but cannot be parsed IS a failure, and is reported as
    // one rather than being silently replaced with a fresh personality.
    [[nodiscard]] bool Load(IdentitySnapshot& outSnapshot, std::string& outError) const;
    [[nodiscard]] bool Save(const IdentitySnapshot& snapshot, std::string& outError) const;

    [[nodiscard]] std::filesystem::path Path() const { return storePath; }

    // Version of the file currently on disk, or nullopt when there is none. Exposed so a
    // migration can be decided without a full load.
    [[nodiscard]] std::optional<int> StoredVersion() const;

private:
    std::filesystem::path storePath;
    mutable std::mutex mutex;
};

} // namespace revia::identity
