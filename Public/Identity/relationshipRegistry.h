#pragma once

#include "Identity/identityStore.h"
#include "Identity/preferenceState.h"
#include "Identity/relationshipState.h"

#include <filesystem>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::identity
{

// Platform-namespaced person identifiers keep unrelated accounts separate.
// Anonymous local identity records history before an introduction.
[[nodiscard]] std::string LocalUserEntityId();
[[nodiscard]] std::string AdapterEntityId(const std::string& source, const std::string& author);

// Thread-safe per-entity relationship state updated only through runtime-observed RelationshipEvent.
// Does not infer evidence, appraise emotion or call a model.
class RelationshipRegistry
{
public:
    // Takes a path rather than a store: IdentityStore owns a mutex and so cannot be
    // moved, and constructing it in place keeps that detail out of every caller.
    explicit RelationshipRegistry(std::filesystem::path path = "RuntimeData/Identity/identity.json");

    // Reads persisted relationships. A missing file is a first run, not a failure; a
    // corrupt one is reported so the caller can refuse to overwrite it.
    bool Load(std::string& outError);
    bool Save(std::string& outError) const;

    // The relationship for an entity, creating a neutral one on first contact. Creating
    // is not the same as liking: a new entity starts at zero affinity and low trust,
    // and has to earn everything from there.
    [[nodiscard]] RelationshipState Get(const std::string& entityId);
    [[nodiscard]] std::optional<RelationshipState> Find(const std::string& entityId) const;
    [[nodiscard]] std::vector<RelationshipState> All() const;

    // Applies evidence and returns the updated relationship.
    RelationshipState Apply(const RelationshipEvent& event);

    // One bounded cooling step per person after a quiet interval. Clock metadata
    // stays in memory; a restart does not invent time spent apart.
    void SettleAll(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now(),
        std::chrono::milliseconds quietInterval = std::chrono::minutes(5));

    void SetDisplayName(const std::string& entityId, const std::string& displayName);

    // The id a named local speaker is stored under.
    [[nodiscard]] static std::string NamedLocalEntityId(const std::string& name);

    // Returns the named local entity; the first introduction adopts anonymous local history.
    // Later introductions keep distinct neutral-start relationships.
    std::string ResolveNamedLocalSpeaker(const std::string& name);

    // Who is at the keyboard when a session starts, before anyone has said a name: the
    // one named local person, when only one has ever used it; otherwise nobody in
    // particular, until someone introduces themselves. Starting every session as the
    // anonymous entity meant a person who had introduced themselves once was a stranger
    // again after each restart, and their later conversations built up a second,
    // nameless relationship beside their real one.
    [[nodiscard]] std::string DefaultLocalSpeaker() const;

    // Development and mood live in the same file, so the registry carries them through
    // a load/save cycle rather than letting a relationship save silently discard them.
    [[nodiscard]] DevelopmentState Development() const;
    void SetDevelopment(const DevelopmentState& development);
    // Replaces where she started while keeping what she has earned. The profile owns the
    // baseline and reapplies it at every startup and profile change; delta is runtime
    // state and must survive that, or editing a profile would erase her development.
    void SetDevelopmentBaseline(const TraitVector& baseline);
    // Appended, never replaced. The history is the explanation for how she got here, and
    // a personality change with no recorded reason is indistinguishable from a bug.
    void RecordDevelopmentChange(const DevelopmentChange& change);
    [[nodiscard]] std::vector<DevelopmentChange> DevelopmentHistory() const;
    [[nodiscard]] emotion::MoodState Mood() const;
    void SetMood(const emotion::MoodState& mood);

    // Preserves opinions in the shared identity snapshot and applies bounded observation-driven changes.
    // Callers supply evidence rather than assigning preference strength.
    Preference ReinforcePreference(const std::string& subject, bool positive, PreferenceSource source);
    // Inserts a profile-declared preference only when she does not already hold one for
    // that subject. Earned opinion outranks an authored starting point.
    void SeedPreferences(const std::vector<std::pair<std::string, float>>& declared);
    [[nodiscard]] std::vector<Preference> Preferences() const;
    [[nodiscard]] std::vector<Preference> StrongestPreferences(std::size_t limit) const;

    [[nodiscard]] std::size_t Count() const;

private:
    mutable std::mutex mutex;
    IdentityStore store;
    IdentitySnapshot snapshot;
    std::map<std::string, std::chrono::steady_clock::time_point> frictionUpdatedAt;
    // Kept alongside the snapshot rather than inside it: the set owns the bounded update
    // rules, and the snapshot is the plain data those rules produce.
    PreferenceSet preferences;
};

} // namespace revia::identity
