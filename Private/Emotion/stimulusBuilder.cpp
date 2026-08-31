#include "Emotion/stimulusBuilder.h"

#include <algorithm>

namespace revia::emotion
{

Stimulus BuildConversationStimulus(
    const std::string& entityId,
    const identity::ConversationSignals& signals)
{
    Stimulus stimulus;
    stimulus.source = StimulusSource::Conversation;
    stimulus.subjectId = entityId;
    stimulus.userCaused = true;
    stimulus.importance = std::clamp(signals.importance, 0.0F, 1.0F);
    // Keyword reading, not measurement. The appraisal scales by this, so an inference
    // the reader is unsure of produces a correspondingly weaker feeling.
    stimulus.certainty = 0.6F;

    if (signals.hostileTowardRevia)
    {
        stimulus.eventType = "hostile_remark";
        stimulus.description = "a remark aimed at her";
        stimulus.valence = -0.8F;
        stimulus.importance = std::max(stimulus.importance, 0.6F);
    }
    else if (signals.expressedAppreciation)
    {
        stimulus.eventType = "appreciation";
        stimulus.description = "being thanked";
        stimulus.valence = 0.7F;
    }
    else if (signals.repeatedCorrection)
    {
        // She missed something and is being told so. Mildly negative and self-caused:
        // this is her failing to listen, not the user being unpleasant.
        stimulus.eventType = "repeated_correction";
        stimulus.description = "having to be corrected again";
        stimulus.valence = -0.35F;
        stimulus.selfCaused = true;
        stimulus.failure = 0.4F;
    }
    else if (signals.collaborative)
    {
        stimulus.eventType = "collaboration";
        stimulus.description = "working on something together";
        stimulus.valence = 0.4F;
    }
    else
    {
        stimulus.eventType = "message";
        stimulus.description = "an ordinary exchange";
        // Deliberately flat. Most turns are not emotional events, and giving every
        // message a valence is how a companion ends up visibly reacting to "ok".
        stimulus.valence = 0.0F;
    }

    if (!signals.succeeded)
    {
        // The turn itself went wrong, which is hers regardless of what was said.
        stimulus.selfCaused = true;
        stimulus.failure = std::max(stimulus.failure, 0.5F);
        stimulus.valence = std::min(stimulus.valence, -0.2F);
    }
    return stimulus;
}

Stimulus BuildGoalStimulus(
    const bool succeeded,
    const bool exhausted,
    const bool blocked,
    const std::size_t actionsSpent,
    const std::size_t retriesSpent,
    const std::string& summary)
{
    Stimulus stimulus;
    stimulus.source = StimulusSource::Goal;
    stimulus.description = summary;
    // She chose the steps and spent the budget, so a goal outcome is hers. This is what
    // makes a failed goal frustrating rather than merely unfortunate.
    stimulus.selfCaused = true;
    // Longer goals cost more and matter more, capped so a twenty-step goal is not four
    // times as important as a five-step one.
    stimulus.importance = std::clamp(
        0.35F + 0.05F * static_cast<float>(actionsSpent), 0.0F, 0.85F);

    if (succeeded)
    {
        stimulus.eventType = "goal_succeeded";
        stimulus.success = 0.9F;
        stimulus.valence = 0.6F;
        // Retries mean it did not go smoothly, and a hard-won success is the kind worth
        // being pleased about.
        stimulus.novelty = retriesSpent > 0 ? 0.55F : 0.2F;
    }
    else if (blocked)
    {
        // Policy or a missing permission stopped it. Nothing broke, and it is not hers.
        stimulus.eventType = "goal_blocked";
        stimulus.failure = 0.4F;
        stimulus.valence = -0.3F;
        stimulus.selfCaused = false;
    }
    else
    {
        stimulus.eventType = exhausted ? "goal_exhausted" : "goal_failed";
        stimulus.failure = exhausted ? 0.7F : 0.85F;
        stimulus.valence = -0.6F;
    }
    return stimulus;
}

Stimulus BuildDiscoveryStimulus(
    const std::string& description,
    const float novelty,
    const float importance)
{
    Stimulus stimulus;
    stimulus.source = StimulusSource::Research;
    stimulus.eventType = "discovery";
    stimulus.description = description;
    stimulus.novelty = std::clamp(novelty, 0.0F, 1.0F);
    stimulus.importance = std::clamp(importance, 0.0F, 1.0F);
    stimulus.valence = 0.3F;
    return stimulus;
}

} // namespace revia::emotion
