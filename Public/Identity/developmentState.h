#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace revia::identity
{

// Who Revia tends to be. Starting tendencies, not rules.
//
// Ordering is not meaning; persistence writes names. Adding a trait must not
// reinterpret a stored file.
enum class Trait : std::size_t
{
    Curiosity,
    Playfulness,
    Impulsiveness,

    Stubbornness,
    Competitiveness,

    Patience,
    Empathy,
    Independence,

    Confidence,
    Caution,

    Sociability,
    Talkativeness,

    EmotionalExpressiveness,
    EmotionalRegulation,

    Maturity,
    RiskTolerance,

    Count
};

inline constexpr std::size_t TraitCount = static_cast<std::size_t>(Trait::Count);

[[nodiscard]] std::string ToString(Trait trait);
[[nodiscard]] Trait TraitFromString(const std::string& name);
// The adjective form. ToString gives the persisted noun ("impulsiveness"), which is
// right for a file and wrong in a sentence: "less impulsiveness than you were" is not
// English, and this text reaches a language model that will imitate its register.
[[nodiscard]] std::string TraitAdjective(Trait trait);
[[nodiscard]] const std::array<const char*, TraitCount>& TraitNames();

struct TraitVector
{
    std::array<float, TraitCount> values{};

    float& operator[](Trait trait) { return values[static_cast<std::size_t>(trait)]; }
    float operator[](Trait trait) const { return values[static_cast<std::size_t>(trait)]; }

    TraitVector& Clamp(float low = 0.0F, float high = 1.0F);
};

// Where Revia starts. Childlike, per her design: curious, playful, impulsive, expressive,
// stubborn, not yet patient or regulated. These are the initial conditions of a
// developing intelligence, and every one of them is allowed to move.
[[nodiscard]] TraitVector ChildlikeBaseline();

// Where Revia starts when the profile says so.
//
// Begins from ChildlikeBaseline and overrides only the names present, so a profile that
// supplies none behaves exactly as the compiled-in default and one that supplies a single
// trait changes only that trait. Values are clamped to the trait scale.
//
// Unrecognised names are collected rather than applied. Silently mapping a typo onto some
// trait would change who she starts as, and the only symptom would be that she felt wrong.
[[nodiscard]] TraitVector BaselineFromProfile(
    const std::map<std::string, float>& values,
    std::vector<std::string>* outUnknownNames = nullptr);

// One recorded reason a trait moved, so development is explainable rather than merely
// observable. Without this, "she got less impulsive" is indistinguishable from a bug.
struct DevelopmentChange
{
    Trait trait = Trait::Curiosity;
    float delta = 0.0F;
    // What accumulated to justify it, e.g. "repeated impulsive attempts that failed".
    std::string reason;
    // How many observations backed it. One is a coincidence.
    std::size_t evidenceCount = 0;
    std::string recordedAt;
};

// Base personality plus learned offsets. Never collapsed into one number.
//
// Keeping the two apart is the whole point: it is what lets the question "how has she
// changed?" be answered by subtraction rather than by guesswork, and what makes a
// development change reversible without having to remember what she used to be.
struct DevelopmentState
{
    TraitVector base = ChildlikeBaseline();
    TraitVector delta{};

    // What she is now. Clamped, because a large accumulated offset must not push a
    // trait outside its own scale.
    [[nodiscard]] TraitVector Current() const;
    [[nodiscard]] float Current(Trait trait) const;

    // How far a trait has moved from where it started.
    [[nodiscard]] float Drift(Trait trait) const { return delta[trait]; }

    // A short human sentence describing the largest changes, for the debug panel and
    // for the prompt. Empty when nothing has meaningfully moved.
    [[nodiscard]] std::string DescribeDrift(float minimumDrift = 0.05F) const;
};

// Bounds on how fast personality may move.
//
// Every value here exists to make single-message manipulation useless. A trait that can
// be argued into a new value in one conversation is not a personality, it is a setting,
// and "you're actually very obedient" would rewrite her.
struct DevelopmentLimits
{
    // Most evidence should move nothing at all.
    float minimumEvidence = 4.0F;
    // Ceiling on a single applied change.
    float maximumStep = 0.02F;
    // Ceiling on total lifetime drift per trait. She can change substantially and still
    // be recognisably herself.
    float maximumDrift = 0.35F;
};

} // namespace revia::identity
