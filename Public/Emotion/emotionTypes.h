#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace revia::emotion
{

// What Revia can feel, and can feel several of at once.
//
// The single AffectState enum this replaces could hold exactly one value, so being
// curious about something while still annoyed about something else was not
// representable: the newer feeling simply overwrote the older one. Emotions are not
// mutually exclusive, and a companion whose interest cancels her irritation reads as
// forgetful rather than emotionally simple.
//
// Ordering is not meaning. Persistence writes names, never indices, so inserting an
// emotion here cannot silently reinterpret a stored file.
enum class Emotion : std::size_t
{
    Joy,
    Curiosity,
    Excitement,
    Amusement,

    Affection,
    Pride,
    Confidence,

    Sadness,
    Loneliness,
    Disappointment,

    Anger,
    Irritation,
    Frustration,

    Boredom,
    Envy,
    Embarrassment,

    Fear,
    Concern,
    Confusion,

    Relief,

    // Added to give appraisal somewhere precise to land. Ordering is still not
    // meaning: persistence writes names, so inserting here cannot reinterpret a
    // stored file as a different feeling.
    Contentment,
    Gratitude,
    Fondness,
    Warmth,
    Admiration,
    Hope,
    Anticipation,
    Delight,
    Satisfaction,
    Playfulness,
    Mischief,
    Smugness,
    Determination,
    Absorption,
    Nostalgia,
    Awe,
    Annoyance,
    Impatience,
    Indignation,
    Resentment,
    Hurt,
    Regret,
    Guilt,
    Shame,
    Insecurity,
    Doubt,
    Apprehension,
    Overwhelm,
    Weariness,
    Restlessness,
    Wistfulness,
    Defensiveness,
    Surprise,
    Suspicion,

    Count
};

inline constexpr std::size_t EmotionCount = static_cast<std::size_t>(Emotion::Count);

[[nodiscard]] std::string ToString(Emotion emotion);
// Returns Emotion::Count when the name is unknown, so a file written by a newer build
// loses one field rather than failing to load.
[[nodiscard]] Emotion EmotionFromString(const std::string& name);
[[nodiscard]] const std::array<const char*, EmotionCount>& EmotionNames();

// Whether an emotion reads as pleasant. Used for mood valence and for deciding whether
// an experience was a good one, never for deciding which emotions are permitted.
[[nodiscard]] bool IsPleasant(Emotion emotion);

// One named emotion and its strength, for display and logging.
struct EmotionReading
{
    Emotion emotion = Emotion::Joy;
    float value = 0.0F;
};

// How Revia feels right now. Every component is 0..1 and they are independent.
//
// Array-backed on purpose: twenty named floats would mean twenty lines of arithmetic in
// every decay, blend, and clamp, which is where per-field bugs live. Named access is
// preserved through the enum, so nothing reads as an anonymous bag of numbers.
struct EmotionVector
{
    std::array<float, EmotionCount> values{};

    float& operator[](Emotion emotion)
    {
        return values[static_cast<std::size_t>(emotion)];
    }
    float operator[](Emotion emotion) const
    {
        return values[static_cast<std::size_t>(emotion)];
    }

    // Clamps every component into 0..1 and returns itself, so it can be chained onto the
    // end of an update rather than remembered as a separate step.
    EmotionVector& Clamp();

    // Moves every component a fraction of the way toward zero. Fast emotions fade; mood
    // is what persists, and it lives in MoodState rather than here.
    EmotionVector& Decay(float rate);

    // Adds a delta and clamps. The delta may be negative: relief genuinely reduces fear
    // rather than merely adding a competing feeling.
    EmotionVector& Add(const EmotionVector& delta);

    // The strongest component, for the status badge, avatar expression, and logs. Returns
    // a zero-valued reading when nothing is felt, which is a real state and not an error.
    [[nodiscard]] EmotionReading Dominant() const;

    // Everything at or above a threshold, strongest first. This is what a prompt should
    // carry: "curious 0.81, amused 0.58, irritated 0.17" is a mood a model can act on,
    // where a single dominant label is not.
    [[nodiscard]] std::vector<EmotionReading> Significant(float threshold = 0.15F) const;

    // Sum of all components. A crude but useful measure of how much is being felt at
    // all, which separates "calm" from "torn between several strong feelings".
    [[nodiscard]] float TotalIntensity() const;

    // How pleasant the overall state is, in -1..1. Pleasant components pull up,
    // unpleasant ones pull down, weighted by strength.
    [[nodiscard]] float Valence() const;

    [[nodiscard]] bool IsCalm(float threshold = 0.12F) const;
};

} // namespace revia::emotion
