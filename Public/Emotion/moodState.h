#pragma once

#include "Emotion/emotionTypes.h"

namespace revia::emotion
{

// The slow layer under the fast one.
//
// Emotion answers "how does she feel about what just happened"; mood answers "how has
// she been". They need separate lifetimes: anger that fades in five minutes is an
// emotion, and the irritability it leaves behind for an hour is a mood. Collapsing them
// gives either a companion who forgets a bad afternoon the moment you change the
// subject, or one who stays furious about it all week.
//
// Mood is fed by emotion and then feeds back into appraisal, which is what makes a mild
// annoyance land harder on a day that has already gone badly.
struct MoodState
{
    // -1 sustained low .. +1 sustained good. Slower and smaller in range than the
    // valence of any single emotion.
    float valence = 0.0F;
    // 0 flat .. 1 keyed up. Separate from valence because excited and distressed are
    // both high-energy and nothing else about them is alike.
    float energy = 0.45F;
    // How short the fuse currently is. Read by appraisal, not just reported.
    float irritability = 0.0F;
    // Appetite for interaction. Spending it is what makes playing cost something.
    float sociability = 0.6F;

    // Where mood returns to when nothing is happening. Not zero: a person at rest is
    // not emotionless, and a baseline that can itself drift slowly over long periods is
    // how a temperament shows up in the numbers.
    float baselineValence = 0.0F;
    float baselineEnergy = 0.45F;
    float baselineSociability = 0.6F;

    [[nodiscard]] bool IsLow(float threshold = -0.3F) const { return valence <= threshold; }
    [[nodiscard]] bool IsIrritable(float threshold = 0.45F) const
    {
        return irritability >= threshold;
    }
};

// How fast mood moves. Every rate is per integration step rather than per second so the
// caller owns the clock and the controller stays testable without sleeping.
struct MoodDynamics
{
    // Fraction of the gap to the baseline closed per step. Deliberately small: mood that
    // returns to neutral quickly is just a slower emotion.
    float returnRate = 0.04F;
    // How much of the current emotional state is absorbed into mood per step. Smaller
    // than returnRate would make mood unreachable; larger makes it whiplash.
    float absorbRate = 0.08F;
    // Ceiling on how far mood can be pushed from its baseline in one step, so a single
    // enormous event cannot rewrite the day.
    float maximumStep = 0.06F;
    // Mood saturates rather than running away. Repeated bad news deepens a low mood with
    // diminishing returns instead of driving it to -1 and pinning it there.
    float saturation = 0.85F;
};

// Integrates emotion into mood, and lets mood decay back toward its baseline.
//
// Owns no clock and no thread. The caller decides when a step happens, which keeps the
// dynamics a pure function of their inputs and testable without waiting.
class MoodController
{
public:
    explicit MoodController(MoodDynamics dynamics = {});

    // One integration step. Pulls mood toward the baseline, then absorbs the current
    // emotional state, then applies saturation and the per-step ceiling.
    MoodState Integrate(MoodState mood, const EmotionVector& emotion) const;

    // Decay with no emotion present, for idle time.
    [[nodiscard]] MoodState Settle(MoodState mood) const;

    // How much a mood should scale an incoming feeling. Above 1 when mood amplifies
    // (an irritable day makes irritation land harder), below 1 when it dampens.
    [[nodiscard]] float AppraisalGain(const MoodState& mood, Emotion emotion) const;

    [[nodiscard]] const MoodDynamics& Dynamics() const { return dynamics; }

private:
    MoodDynamics dynamics;
};

} // namespace revia::emotion
