#include "Emotion/appraisalContext.h"

#include <algorithm>

namespace revia::emotion
{

namespace
{
    float Clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

    // How much a stimulus bears on something Revia was actually doing. Goal and action
    // outcomes are relevant by construction; a passing perception event usually is not.
    float DeriveGoalRelevance(const Stimulus& stimulus)
    {
        switch (stimulus.source)
        {
            case StimulusSource::Goal:
            case StimulusSource::Action:
                return Clamp01(0.6F + 0.4F * stimulus.importance);
            case StimulusSource::Research:
                return Clamp01(0.45F + 0.35F * stimulus.importance);
            case StimulusSource::Conversation:
            case StimulusSource::Relationship:
                return Clamp01(0.3F + 0.4F * stimulus.importance);
            case StimulusSource::Perception:
            case StimulusSource::Environment:
                return Clamp01(0.15F + 0.3F * stimulus.importance);
            case StimulusSource::Memory:
            case StimulusSource::Internal:
                return Clamp01(0.2F + 0.3F * stimulus.importance);
        }
        return 0.4F;
    }

    // Expectedness is the inverse of novelty, softened by how confident the runtime is
    // that the event happened as described. Something uncertain is not a surprise; it is
    // merely unclear, and those produce different feelings.
    float DeriveExpectedness(const Stimulus& stimulus)
    {
        return Clamp01((1.0F - Clamp01(stimulus.novelty)) * Clamp01(stimulus.certainty));
    }

    // Whether anything could have been done about it. Her own actions are controllable
    // almost by definition; the weather of the desktop is not.
    float DeriveControllability(const Stimulus& stimulus)
    {
        if (stimulus.selfCaused)
        {
            return 0.75F;
        }
        switch (stimulus.source)
        {
            case StimulusSource::Action:
            case StimulusSource::Goal:
                return 0.5F;
            case StimulusSource::Environment:
            case StimulusSource::Perception:
                return 0.15F;
            default:
                return 0.35F;
        }
    }

    // Anyone watching. This is the difference between a private failure that is annoying
    // and a witnessed one that is also embarrassing.
    float DeriveSocialImportance(
        const Stimulus& stimulus,
        const identity::RelationshipState* relationship)
    {
        if (stimulus.subjectId.empty())
        {
            return stimulus.userCaused ? 0.4F : 0.0F;
        }
        if (relationship == nullptr)
        {
            return 0.45F;
        }
        // Failing in front of someone whose opinion she values costs more. Respect
        // rather than affinity: she can be indifferent to someone and still not want to
        // look foolish in front of them.
        return Clamp01(
            0.25F + 0.45F * relationship->respect + 0.3F * relationship->familiarity);
    }
}

AppraisalContext BuildAppraisalContext(
    const Stimulus& stimulus,
    const identity::DevelopmentState& development,
    const MoodState& mood,
    const EmotionVector& currentEmotion,
    const identity::RelationshipState* relationship,
    std::vector<RelevantMemory> memories)
{
    AppraisalContext context;
    context.development = development;
    context.mood = mood;
    context.currentEmotion = currentEmotion;

    if (relationship != nullptr)
    {
        context.relationship = *relationship;
        context.hasRelationship = true;
    }

    context.novelty = Clamp01(stimulus.novelty);
    context.expectedness = DeriveExpectedness(stimulus);
    context.goalRelevance = DeriveGoalRelevance(stimulus);
    context.controllability = DeriveControllability(stimulus);
    context.selfResponsibility = stimulus.selfCaused ? 1.0F : 0.0F;
    context.socialImportance = DeriveSocialImportance(stimulus, relationship);

    // Strongest associations first, then truncated. An unbounded memory list would make
    // every appraisal proportional to how much she happens to remember.
    std::sort(memories.begin(), memories.end(),
        [](const RelevantMemory& left, const RelevantMemory& right)
        {
            return left.similarity * left.importance >
                right.similarity * right.importance;
        });
    if (memories.size() > AppraisalContext::maximumMemories)
    {
        memories.resize(AppraisalContext::maximumMemories);
    }
    context.memories = std::move(memories);

    // Past episodes temper expectation. If similar situations went badly before, this
    // one is less of a surprise when it does too -- which is what stops a familiar
    // disappointment from landing like a fresh shock every time.
    if (!context.memories.empty())
    {
        float weight = 0.0F;
        float recalledValence = 0.0F;
        for (const RelevantMemory& memory : context.memories)
        {
            const float influence = Clamp01(memory.similarity) * Clamp01(memory.importance);
            weight += influence;
            recalledValence += influence * memory.pastValence;
        }
        if (weight > 0.0F)
        {
            recalledValence /= weight;
            const bool matchesPast = (recalledValence < 0.0F && stimulus.failure > 0.0F) ||
                (recalledValence > 0.0F && stimulus.success > 0.0F);
            if (matchesPast)
            {
                const float familiarPattern = std::min(0.3F, weight * 0.15F);
                context.expectedness = Clamp01(context.expectedness + familiarPattern);
                context.novelty = Clamp01(context.novelty - familiarPattern);
            }
        }
    }
    return context;
}

} // namespace revia::emotion
