#pragma once

#include "Identity/identityStore.h"
#include "Identity/preferenceState.h"
#include "Identity/relationshipState.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::identity
{

// Who Revia is talking to, as a stable identifier.
//
// Entity ids are namespaced by where the person came from, because "quentin" on a
// stream and "quentin" at this keyboard are not known to be the same person and
// assuming they are would merge two relationships that were earned separately.
//
// The local user gets a fixed id rather than a name. Revia may not know what to call
// them yet, and a relationship that only begins once someone introduces themselves
// would lose every interaction before that point.
[[nodiscard]] std::string LocalUserEntityId();
[[nodiscard]] std::string AdapterEntityId(
    const std::string& source, const std::string& author);

// The live, per-entity relationship database.
//
// Single purpose: it remembers how Revia stands with each person and applies evidence to
// that. It does not decide what the evidence is, does not appraise, and cannot reach a
// model. Deltas come only from RelationshipEvent, which the runtime constructs from
// observable signals -- so "we are best friends now" cannot become true by being said.
//
// Thread-safe: conversation, adapters, and idle settling all touch it from different
// threads.
class RelationshipRegistry
{
public:
    // Takes a path rather than a store: IdentityStore owns a mutex and so cannot be
    // moved, and constructing it in place keeps that detail out of every caller.
    explicit RelationshipRegistry(
        std::filesystem::path path = "RuntimeData/Identity/identity.json");

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

    // Time passing. Friction cools, grievance mostly does not.
    void SettleAll();

    void SetDisplayName(const std::string& entityId, const std::string& displayName);

    // The id a named local speaker is stored under.
    [[nodiscard]] static std::string NamedLocalEntityId(const std::string& name);

    // Resolves who is speaking at the keyboard once they give a name.
    //
    // The first person to introduce themselves inherits the anonymous local history,
    // because they are almost certainly whoever has been talking all along and throwing
    // that away would be worse than the small risk of attributing it wrongly. Anyone who
    // introduces themselves afterwards becomes their own entity with their own
    // relationship, starting neutral.
    //
    // Returns the entity id the caller should attribute this turn to.
    std::string ResolveNamedLocalSpeaker(const std::string& name);

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

    // Opinions live in the same file, so the registry carries them through a load/save
    // cycle for the same reason it carries development: a relationship save must not
    // silently discard what she likes.
    //
    // Evidence in, bounded change out. Callers supply an observation, never a value:
    // letting a caller set strength directly would make one sentence able to install a
    // lifelong taste, which is what the bounded step exists to prevent.
    Preference ReinforcePreference(
        const std::string& subject, bool positive, PreferenceSource source);
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
    // Kept alongside the snapshot rather than inside it: the set owns the bounded update
    // rules, and the snapshot is the plain data those rules produce.
    PreferenceSet preferences;
};

} // namespace revia::identity
