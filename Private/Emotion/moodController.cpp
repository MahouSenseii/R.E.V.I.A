#include "Emotion/moodState.h"
#include "Emotion/stimulus.h"

#include <algorithm>
#include <cmath>

namespace revia::emotion
{

namespace
{
    float Approach(const float value, const float target, const float rate, const float ceiling)
    {
        const float gap = (target - value) * std::clamp(rate, 0.0F, 1.0F);
        return value + std::clamp(gap, -ceiling, ceiling);
    }

    // Pushes hard near zero and barely at all near the limit, so repeated bad days
    // deepen a low mood with diminishing returns instead of pinning it at -1.
    float Saturate(const float value, const float limit)
    {
        const float bound = std::max(limit, 0.05F);
        return bound * std::tanh(value / bound);
    }

    // Emotions that specifically feed the short-fuse axis, as opposed to merely being
    // unpleasant. Sadness lowers mood without making her snappish; irritation does both.
    float IrritabilityContribution(const EmotionVector& emotion)
    {
        return 0.5F * emotion[Emotion::Irritation] +
            0.4F * emotion[Emotion::Anger] +
            0.35F * emotion[Emotion::Frustration] +
            0.2F * emotion[Emotion::Boredom];
    }

    float EnergyContribution(const EmotionVector& emotion)
    {
        const float keyedUp = emotion[Emotion::Excitement] + emotion[Emotion::Anger] +
            emotion[Emotion::Fear] + emotion[Emotion::Curiosity];
        const float flattened = emotion[Emotion::Sadness] + emotion[Emotion::Boredom] +
            emotion[Emotion::Loneliness];
        return 0.25F * (keyedUp - flattened);
    }

    float SociabilityContribution(const EmotionVector& emotion)
    {
        const float drawn = emotion[Emotion::Affection] + emotion[Emotion::Amusement] +
            emotion[Emotion::Joy];
        const float withdrawn = emotion[Emotion::Embarrassment] + emotion[Emotion::Anger] +
            emotion[Emotion::Sadness] + emotion[Emotion::Fear];
        return 0.22F * (drawn - withdrawn);
    }
}

MoodController::MoodController(MoodDynamics inputDynamics)
    : dynamics(inputDynamics)
{
}

MoodState MoodController::Settle(MoodState mood) const
{
    mood.valence = Approach(
        mood.valence, mood.baselineValence, dynamics.returnRate, dynamics.maximumStep);
    mood.energy = Approach(
        mood.energy, mood.baselineEnergy, dynamics.returnRate, dynamics.maximumStep);
    mood.sociability = Approach(
        mood.sociability, mood.baselineSociability, dynamics.returnRate, dynamics.maximumStep);
    // Irritability always drains toward calm; there is no baseline grudge.
    mood.irritability = Approach(
        mood.irritability, 0.0F, dynamics.returnRate, dynamics.maximumStep);

    mood.valence = std::clamp(mood.valence, -1.0F, 1.0F);
    mood.energy = std::clamp(mood.energy, 0.0F, 1.0F);
    mood.sociability = std::clamp(mood.sociability, 0.0F, 1.0F);
    mood.irritability = std::clamp(mood.irritability, 0.0F, 1.0F);
    return mood;
}

MoodState MoodController::Integrate(MoodState mood, const EmotionVector& emotion) const
{
    // Settle first, then absorb. Doing it the other way round would let a step both
    // decay away the feeling just added and count it, which reads as mood ignoring
    // strong emotions that arrive during quiet stretches.
    mood = Settle(mood);

    const float absorb = std::clamp(dynamics.absorbRate, 0.0F, 1.0F);
    const float ceiling = dynamics.maximumStep;

    // Emotional valence is weighted by how much is being felt at all, so a faint
    // pleasant flicker does not move mood as much as a strong one.
    const float felt = std::clamp(emotion.TotalIntensity(), 0.0F, 3.0F) / 3.0F;
    const float valenceTarget = emotion.Valence() * felt;
    mood.valence += std::clamp(valenceTarget * absorb, -ceiling, ceiling);
    mood.energy += std::clamp(EnergyContribution(emotion) * absorb, -ceiling, ceiling);
    mood.sociability +=
        std::clamp(SociabilityContribution(emotion) * absorb, -ceiling, ceiling);
    mood.irritability +=
        std::clamp(IrritabilityContribution(emotion) * absorb, -ceiling, ceiling);

    mood.valence = Saturate(mood.valence, dynamics.saturation);
    mood.energy = std::clamp(mood.energy, 0.0F, 1.0F);
    mood.sociability = std::clamp(mood.sociability, 0.0F, 1.0F);
    mood.irritability = std::clamp(mood.irritability, 0.0F, 1.0F);
    return mood;
}

float MoodController::AppraisalGain(const MoodState& mood, const Emotion emotion) const
{
    // Mood does not decide what is felt; it decides how hard it lands. Bounded tightly
    // so a bad day colours a reaction rather than replacing it.
    float gain = 1.0F;
    switch (emotion)
    {
        case Emotion::Irritation:
        case Emotion::Anger:
        case Emotion::Frustration:
            gain += 0.45F * mood.irritability;
            // A good mood genuinely absorbs a small annoyance.
            gain -= 0.20F * std::max(0.0F, mood.valence);
            break;
        case Emotion::Sadness:
        case Emotion::Loneliness:
        case Emotion::Disappointment:
            gain += 0.40F * std::max(0.0F, -mood.valence);
            break;
        case Emotion::Joy:
        case Emotion::Amusement:
        case Emotion::Excitement:
            // Hard to be delighted while worn down, and easier when already up.
            gain += 0.30F * std::max(0.0F, mood.valence);
            gain -= 0.35F * std::max(0.0F, -mood.valence);
            break;
        case Emotion::Curiosity:
            // Curiosity needs energy more than it needs cheerfulness.
            gain += 0.30F * (mood.energy - 0.45F);
            break;
        case Emotion::Affection:
        case Emotion::Boredom:
            gain += emotion == Emotion::Affection
                ? 0.30F * (mood.sociability - 0.6F)
                : 0.30F * (0.6F - mood.sociability);
            break;
        default:
            break;
    }
    return std::clamp(gain, 0.4F, 1.8F);
}

std::string ToString(const StimulusSource source)
{
    switch (source)
    {
        case StimulusSource::Conversation: return "conversation";
        case StimulusSource::Relationship: return "relationship";
        case StimulusSource::Perception: return "perception";
        case StimulusSource::Memory: return "memory";
        case StimulusSource::Goal: return "goal";
        case StimulusSource::Action: return "action";
        case StimulusSource::Research: return "research";
        case StimulusSource::Environment: return "environment";
        case StimulusSource::Internal: return "internal";
    }
    return "internal";
}

StimulusSource StimulusSourceFromString(const std::string& name)
{
    if (name == "conversation") return StimulusSource::Conversation;
    if (name == "relationship") return StimulusSource::Relationship;
    if (name == "perception") return StimulusSource::Perception;
    if (name == "memory") return StimulusSource::Memory;
    if (name == "goal") return StimulusSource::Goal;
    if (name == "action") return StimulusSource::Action;
    if (name == "research") return StimulusSource::Research;
    if (name == "environment") return StimulusSource::Environment;
    return StimulusSource::Internal;
}

bool Stimulus::IsMeaningful(const float importanceFloor) const
{
    // Certainty gates alongside importance: a large event the runtime is unsure actually
    // happened should not produce a confident feeling.
    return importance * certainty >= importanceFloor;
}

} // namespace revia::emotion
