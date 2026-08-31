#include "Emotion/emotionModel.h"

#include <algorithm>
#include <memory>

namespace revia::emotion
{

namespace
{
    using identity::Trait;

    float Clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

    // Centred trait influence: a trait at its midpoint changes nothing, and moving away
    // from the middle scales the response either way. This is what keeps personality a
    // multiplier rather than a source of feeling in its own right -- a trait can amplify
    // or damp what an event produced, but it cannot manufacture an emotion from nothing.
    float TraitGain(const float trait, const float strength)
    {
        return 1.0F + strength * (Clamp01(trait) - 0.5F) * 2.0F;
    }
}

EmotionVector RuleEmotionModel::RawResponse(
    const Stimulus& stimulus,
    const AppraisalContext& context)
{
    EmotionVector delta;

    // Everything scales by how much this mattered and how sure we are it happened. An
    // enormous reaction to something the runtime barely believes occurred is exactly the
    // kind of unexplainable mood this architecture exists to prevent.
    const float magnitude = Clamp01(stimulus.importance) * Clamp01(stimulus.certainty);
    if (magnitude <= 0.0F)
    {
        return delta;
    }

    const float success = Clamp01(stimulus.success);
    const float failure = Clamp01(stimulus.failure);
    const float surprise = 1.0F - context.expectedness;
    const float mine = context.selfResponsibility;

    // --- Outcomes -----------------------------------------------------------------
    if (success > 0.0F)
    {
        const float weight = success * magnitude;
        delta[Emotion::Joy] += weight * 0.55F;
        // Pride requires having done it herself. A good outcome she had no hand in is
        // pleasant, not something to be proud of.
        delta[Emotion::Pride] += weight * mine * 0.7F;
        delta[Emotion::Confidence] += weight * mine * context.goalRelevance * 0.5F;
        // Relief is proportional to how much it might not have worked.
        delta[Emotion::Relief] += weight * surprise * context.goalRelevance * 0.6F;
        delta[Emotion::Excitement] += weight * context.novelty * 0.7F;
        // Satisfaction is what a success leaves behind once the excitement passes, and
        // contentment is the quieter version for something that simply went right.
        delta[Emotion::Satisfaction] += weight * context.goalRelevance * 0.5F;
        delta[Emotion::Contentment] += weight * (1.0F - context.novelty) * 0.35F;
        delta[Emotion::Delight] += weight * context.novelty * surprise * 0.5F;
    }

    if (failure > 0.0F)
    {
        const float weight = failure * magnitude;
        // The distinction that matters most: her approach failing is frustrating,
        // something breaking underneath her is worrying. Same event, different feeling.
        delta[Emotion::Frustration] += weight * mine * context.controllability * 0.8F;
        delta[Emotion::Concern] += weight * (1.0F - mine) * 0.7F;
        delta[Emotion::Disappointment] += weight * context.goalRelevance * 0.6F;
        // Expected to work and did not. Surprise at a failure reads as confusion.
        delta[Emotion::Confusion] += weight * context.expectedness * 0.5F;
        // Nothing could be done. That is when a setback turns into sadness rather than
        // annoyance, because annoyance implies something to push against.
        delta[Emotion::Sadness] += weight * (1.0F - context.controllability) * 0.45F;
        // Only in front of someone whose opinion counts.
        delta[Emotion::Embarrassment] += weight * mine * context.socialImportance * 0.5F;
        // Regret and guilt separate "I wish I had chosen differently" from "this cost
        // someone else something", which frustration alone cannot express.
        delta[Emotion::Regret] += weight * mine * context.controllability * 0.45F;
        delta[Emotion::Guilt] += weight * mine * context.socialImportance * 0.4F;
        // Repeated setbacks erode certainty rather than only annoying her.
        delta[Emotion::Doubt] += weight * (1.0F - std::clamp(stimulus.certainty, 0.0F, 1.0F)) * 0.5F;
        delta[Emotion::Insecurity] += weight * mine * (1.0F - context.controllability) * 0.35F;
    }

    // --- Valence ------------------------------------------------------------------
    // Conversation and relationship events usually carry valence without an outcome.
    const float valence = std::clamp(stimulus.valence, -1.0F, 1.0F);
    if (valence > 0.0F)
    {
        const float weight = valence * magnitude;
        delta[Emotion::Joy] += weight * 0.5F;
        delta[Emotion::Amusement] += weight * 0.3F;
        delta[Emotion::Warmth] += weight * 0.35F;
        // Being thanked specifically produces gratitude, which is not the same feeling
        // as simply being pleased that something went well.
        if (stimulus.eventType == "appreciation")
        {
            delta[Emotion::Gratitude] += weight * 0.6F;
            delta[Emotion::Fondness] += weight * 0.3F;
        }
        if (context.hasRelationship)
        {
            // Warmth from someone she is close to is worth more than warmth from a
            // stranger, and affection needs an actual relationship to attach to.
            const float closeness = Clamp01(
                0.5F * context.relationship.familiarity +
                0.5F * std::max(0.0F, context.relationship.affinity));
            delta[Emotion::Affection] += weight * closeness * 0.6F;
        }
    }
    else if (valence < 0.0F)
    {
        const float weight = -valence * magnitude;
        bool absorbed = false;
        if (context.hasRelationship && stimulus.userCaused)
        {
            if (context.relationship.ReadsAsTeasing())
            {
                // A jab from someone she is close to and not currently annoyed with is
                // a joke. Reading it as an attack is what makes a companion exhausting.
                delta[Emotion::Amusement] += weight * 0.55F;
                delta[Emotion::Irritation] += weight * 0.15F;
                absorbed = true;
            }
            else if (context.relationship.trust > 0.55F &&
                context.relationship.affinity > 0.2F)
            {
                // From someone she trusts but is already strained with, the same words
                // hurt rather than anger. Anger is what strangers get.
                delta[Emotion::Hurt] += weight * 0.6F;
                delta[Emotion::Sadness] += weight * 0.55F;
                delta[Emotion::Disappointment] += weight * 0.45F;
                absorbed = true;
            }
        }
        if (!absorbed)
        {
            // Annoyance is the low end and irritation the higher one. Sending every mild
            // negative to irritation made small frictions read as though she were angry.
            delta[Emotion::Annoyance] += weight * 0.55F;
            delta[Emotion::Irritation] += weight * 0.6F;
            delta[Emotion::Anger] += weight * 0.35F * Clamp01(stimulus.importance);
            if (context.hasRelationship && context.relationship.familiarity < 0.2F)
            {
                // No history to soften it and no benefit of the doubt to extend.
                delta[Emotion::Anger] += weight * 0.2F;
            }
        }
    }

    // --- Novelty ------------------------------------------------------------------
    // Curiosity is driven by newness rather than by pleasantness. This is the path by
    // which research and perception make her interested rather than merely informed.
    if (context.novelty > 0.0F)
    {
        delta[Emotion::Curiosity] += context.novelty * magnitude * 0.8F;
        delta[Emotion::Excitement] += context.novelty * magnitude * 0.3F;
        // Surprise is about how far this was from what was expected, which is a
        // different question from whether it was interesting.
        delta[Emotion::Surprise] += context.novelty * surprise * magnitude * 0.7F;
        delta[Emotion::Awe] += context.novelty * magnitude *
            std::clamp(stimulus.importance - 0.6F, 0.0F, 0.4F);
    }
    // Work she is actually engaged in, rather than an outcome. This is what keeps a long
    // task from registering as nothing at all until it finishes.
    if (context.goalRelevance > 0.6F && failure <= 0.0F && success <= 0.0F)
    {
        delta[Emotion::Determination] += context.goalRelevance * magnitude * 0.4F;
        delta[Emotion::Absorption] += context.goalRelevance * magnitude * 0.3F;
    }
    if (stimulus.eventType == "repeated_correction")
    {
        // Being told the same thing again is impatience with herself before it is
        // anything directed outward.
        delta[Emotion::Impatience] += magnitude * 0.5F;
        delta[Emotion::Shame] += magnitude * context.socialImportance * 0.3F;
    }
    // Something happened, it mattered little, and nothing about it was new.
    if (context.novelty < 0.1F && stimulus.importance < 0.35F &&
        success <= 0.0F && failure <= 0.0F)
    {
        delta[Emotion::Boredom] += (0.35F - stimulus.importance) * 0.4F;
        delta[Emotion::Restlessness] += (0.35F - stimulus.importance) * 0.3F;
    }

    return delta.Clamp();
}

EmotionVector RuleEmotionModel::Evaluate(
    const Stimulus& stimulus,
    const AppraisalContext& context) const
{
    EmotionVector delta = RawResponse(stimulus, context);

    // Personality scales the response; it never creates one. Applied here, after the
    // appraisal arithmetic, so who she is stays visible as a separate step rather than
    // being dissolved into coefficients nobody can point at.
    const identity::TraitVector traits = context.development.Current();
    delta[Emotion::Curiosity] *= TraitGain(traits[Trait::Curiosity], 0.5F);
    delta[Emotion::Amusement] *= TraitGain(traits[Trait::Playfulness], 0.5F);
    delta[Emotion::Excitement] *= TraitGain(traits[Trait::Playfulness], 0.3F);
    delta[Emotion::Anger] *= TraitGain(traits[Trait::Stubbornness], 0.3F);
    delta[Emotion::Frustration] *= TraitGain(1.0F - traits[Trait::Patience], 0.5F);
    delta[Emotion::Concern] *= TraitGain(traits[Trait::Caution], 0.4F);
    delta[Emotion::Pride] *= TraitGain(traits[Trait::Competitiveness], 0.4F);
    delta[Emotion::Affection] *= TraitGain(traits[Trait::Empathy], 0.4F);
    delta[Emotion::Embarrassment] *= TraitGain(traits[Trait::Sociability], 0.3F);
    delta[Emotion::Boredom] *= TraitGain(traits[Trait::Curiosity], 0.4F);

    // Regulation damps everything at once. A well-regulated Revia still feels the same
    // things; she feels them less loudly, which is what regulation actually means.
    const float regulation = 1.0F - 0.45F * (Clamp01(traits[Trait::EmotionalRegulation]) - 0.3F);
    // Expressiveness works the other way and is applied on the same pass so the two
    // cannot silently cancel out in a way that hides either.
    const float expressiveness =
        1.0F + 0.35F * (Clamp01(traits[Trait::EmotionalExpressiveness]) - 0.5F);

    // Mood decides how hard each feeling lands. This is the feedback loop: yesterday's
    // accumulated irritation makes today's small annoyance land harder.
    const MoodController moodController;
    for (std::size_t index = 0; index < EmotionCount; ++index)
    {
        const auto emotion = static_cast<Emotion>(index);
        delta.values[index] *= moodController.AppraisalGain(context.mood, emotion) *
            regulation * expressiveness;
    }
    return delta.Clamp();
}

std::unique_ptr<IEmotionModel> MakeDefaultEmotionModel()
{
    // No neural model exists yet. When one does, this is where it is attempted first
    // and where a failed load silently falls back rather than leaving her unable to
    // feel anything at all.
    return std::make_unique<RuleEmotionModel>();
}

} // namespace revia::emotion
