#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace revia::identity
{

// Which way a preference points.
//
// Derived from strength rather than stored beside it. A stored direction can contradict
// its own magnitude, and "likes it, strength -0.8" is a state no caller can act on.
enum class PreferenceDirection
{
    Dislike,
    Neutral,
    Like
};

// How a preference came to exist.
//
// Kept because the same subject learned two different ways deserves different trust:
// something she was told once is not something she worked out repeatedly.
enum class PreferenceSource
{
    Profile,
    Stated,
    Observed,
    Inferred
};

[[nodiscard]] std::string ToString(PreferenceDirection direction);
[[nodiscard]] std::string ToString(PreferenceSource source);
[[nodiscard]] PreferenceSource PreferenceSourceFromString(const std::string& name);

// One thing Revia likes or dislikes, and how sure she is of it.
//
// Strength and confidence are separate on purpose. A strong opinion held on one
// observation and a mild one held on twenty are different states, and collapsing them
// would let a single remark speak with the authority of a long-standing taste.
struct Preference
{
    // Stored lowercase and trimmed; the key is the subject itself, so "Jazz" and "jazz"
    // must not become two opinions that can disagree with each other.
    std::string subject;
    // -1 strong dislike .. +1 strong like. Zero is genuine indifference, not absence.
    float strength = 0.0F;
    float confidence = 0.0F;
    std::size_t evidenceCount = 0;
    std::string lastReinforced;
    PreferenceSource source = PreferenceSource::Inferred;

    [[nodiscard]] PreferenceDirection Direction(float neutralBand = 0.15F) const;
    // Whether she should state this as her own view rather than merely hold it. Below
    // the bar she may still act on it, but asserting a taste she has almost no evidence
    // for is how a model's guess becomes her personality.
    [[nodiscard]] bool WorthStating(
        float minimumConfidence = 0.35F, float neutralBand = 0.15F) const;
};

// Bounds on how fast a taste may form or reverse.
//
// A preference is lighter than a personality trait and is allowed to move faster, but
// not in one step. Without a ceiling, one enthusiastic sentence would install a lifelong
// favourite, which is the failure this exists to prevent.
struct PreferenceLimits
{
    // Most one observation may move a preference.
    float maximumStep = 0.12F;
    // Observations before confidence is full. Confidence rises with evidence, so a new
    // opinion is held tentatively however strongly it was first expressed.
    std::size_t confidentEvidence = 5;
    // Ceiling on how many preferences are retained. The weakest and least evidenced are
    // dropped first: an unbounded set would grow forever and dilute retrieval.
    std::size_t maximumRetained = 200;
};

// What Revia likes and dislikes, and the only thing allowed to change it.
//
// Single purpose: it holds preferences and applies bounded evidence to them. It does not
// decide what counts as evidence, cannot reach a model, and does not store facts --
// design §11 keeps an opinion distinct from a fact, and a preference that could be
// written straight into factual memory would erase that distinction.
class PreferenceSet
{
public:
    // Applies one observation. Direction is the sign of the change, not the resulting
    // value: repeated agreement is what builds a strong taste, so a single call can
    // never do more than maximumStep.
    //
    // Returns the preference as it now stands.
    Preference Reinforce(
        const std::string& subject,
        bool positive,
        PreferenceSource source,
        const std::string& timestamp,
        const PreferenceLimits& limits = {});

    // Adds a preference the profile declares, without disturbing one she already holds.
    //
    // Earned opinion outranks an authored starting point for the same reason development
    // delta survives a baseline change: replacing it would let editing a profile quietly
    // delete what experience produced.
    //
    // Returns true when it was inserted.
    bool SeedFromProfile(const std::string& subject, float strength);

    [[nodiscard]] const Preference* Find(const std::string& subject) const;
    [[nodiscard]] std::vector<Preference> All() const;
    // The strongest held opinions, most strongly held first, bounded for a prompt.
    // Ranked by strength weighted by confidence, so a confident mild taste can outrank a
    // barely evidenced strong one.
    [[nodiscard]] std::vector<Preference> Strongest(std::size_t limit) const;
    [[nodiscard]] std::size_t Count() const { return preferences.size(); }

    void Replace(std::vector<Preference> loaded);

    // Normalises a subject to its storage key.
    [[nodiscard]] static std::string NormaliseSubject(const std::string& subject);

private:
    void Prune(const PreferenceLimits& limits);

    std::vector<Preference> preferences;
};

} // namespace revia::identity
