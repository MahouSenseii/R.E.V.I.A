#include "Emotion/emotionRuntime.h"
#include "Emotion/stimulusBuilder.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace revia::emotion
{

namespace
{
    std::string DescribeReadings(const std::vector<EmotionReading>& readings)
    {
        std::ostringstream description;
        const std::size_t shown = std::min<std::size_t>(readings.size(), 3);
        for (std::size_t index = 0; index < shown; ++index)
        {
            if (index > 0)
            {
                description << (index + 1 == shown ? " and " : ", ");
            }
            description << ToString(readings[index].emotion);
        }
        return description.str();
    }
}

EmotionRuntime::EmotionRuntime(std::unique_ptr<IEmotionModel> inputModel)
    : model(std::move(inputModel))
{
    if (model == nullptr)
    {
        // Never leave her unable to feel anything. A null model would silently turn
        // every event into no reaction, which looks identical to a broken appraisal.
        model = MakeDefaultEmotionModel();
    }
}

std::optional<AppraisalOutcome> EmotionRuntime::Observe(
    const Stimulus& stimulus,
    const identity::DevelopmentState& development,
    const identity::RelationshipState* relationship,
    std::vector<RelevantMemory> memories)
{
    std::lock_guard lock(mutex);
    if (stimulus.source == StimulusSource::Conversation && stimulus.userCaused)
    {
        lastConversation = std::chrono::steady_clock::now();
        quietConversationObserved = false;
        // Renewed contact relieves the quiet feeling even when the message itself is
        // neutral. It is never interpreted as an apology owed to Revia.
        emotion[emotion::Emotion::Loneliness] *= 0.25F;
        emotion[emotion::Emotion::Boredom] *= 0.4F;
    }
    return Appraise(stimulus, development, relationship, std::move(memories));
}

std::optional<AppraisalOutcome> EmotionRuntime::ObserveQuietConversation(
    const identity::DevelopmentState& development,
    const std::chrono::milliseconds quietInterval,
    const bool occupied, const float boredom)
{
    std::lock_guard lock(mutex);
    if (occupied || quietConversationObserved || std::chrono::steady_clock::now() - lastConversation <
        std::max(std::chrono::milliseconds(1), quietInterval))
        return std::nullopt;
    quietConversationObserved = true;
    auto stimulus = BuildQuietConversationStimulus();
    stimulus.description = "A long quiet stretch without conversation or a recent activity";
    const auto traits = development.Current();
    // Independence and current sociability change the response. Being absorbed in
    // something skips this event entirely; quiet does not always mean lonely.
    stimulus.quietConversation = std::clamp(
        0.35F + 0.65F * traits[identity::Trait::Sociability] -
        0.35F * traits[identity::Trait::Independence] + 0.2F * mood.sociability, 0.0F, 1.0F);
    stimulus.idleBoredom = std::clamp(boredom, 0.0F, 1.0F);
    return Appraise(stimulus, development, nullptr, {});
}

std::optional<AppraisalOutcome> EmotionRuntime::Appraise(
    const Stimulus& stimulus,
    const identity::DevelopmentState& development,
    const identity::RelationshipState* relationship,
    std::vector<RelevantMemory> memories)
{
    if (!stimulus.IsMeaningful())
    {
        // Most of what happens is not worth feeling. This is the common path and not a
        // missed case.
        return std::nullopt;
    }

    AppraisalOutcome outcome;
    outcome.stimulus = stimulus;
    outcome.context = BuildAppraisalContext(
        stimulus, development, mood, emotion, relationship, std::move(memories));
    outcome.delta = model->Evaluate(stimulus, outcome.context);
    outcome.modelName = model->Name();

    // An appraisal that produced nothing is reported as nothing rather than as an
    // unchanged state, so a caller cannot mistake "felt nothing" for "was not asked".
    if (outcome.delta.IsCalm(0.01F))
    {
        return std::nullopt;
    }

    emotion.Add(outcome.delta);
    mood = moodController.Integrate(mood, emotion);

    outcome.changed = true;
    outcome.emotion = emotion;
    outcome.mood = mood;

    const std::vector<EmotionReading> produced = outcome.delta.Significant(0.05F);
    std::ostringstream explanation;
    explanation << (stimulus.description.empty()
        ? ToString(stimulus.source) + " event"
        : stimulus.description);
    if (!produced.empty())
    {
        explanation << " \xE2\x80\x94 " << DescribeReadings(produced);
    }
    outcome.explanation = explanation.str();
    // Kept so the prompt can say why she feels this. Only the stimulus's own words:
    // the emotions it produced are already state, and stating them twice would let
    // the two descriptions drift apart. Bounded because it reaches a prompt.
    if (!stimulus.description.empty())
    {
        constexpr std::size_t MaximumCauseCharacters = 160;
        lastCause = stimulus.description.size() > MaximumCauseCharacters
            ? stimulus.description.substr(0, MaximumCauseCharacters)
            : stimulus.description;
        lastCauseAt = std::chrono::steady_clock::now();
    }
    return outcome;
}

void EmotionRuntime::Settle(const float emotionDecayRate)
{
    std::lock_guard lock(mutex);
    // Emotion fades quickly; mood is what is left behind. Integrating the fading emotion
    // rather than settling mood in isolation is what lets an afternoon of small
    // annoyances still add up even as each individual one passes.
    emotion.Decay(emotionDecayRate);
    mood = moodController.Integrate(mood, emotion);
    // A reason for a feeling that has faded is just an old event. Dropping it here
    // keeps her from explaining a mood she is no longer in.
    if (emotion.IsCalm())
    {
        lastCause.clear();
    }
}

EmotionVector EmotionRuntime::Emotion() const
{
    std::lock_guard lock(mutex);
    return emotion;
}

MoodState EmotionRuntime::Mood() const
{
    std::lock_guard lock(mutex);
    return mood;
}

EmotionSnapshot EmotionRuntime::Current() const
{
    std::lock_guard lock(mutex);
    // Expired rather than erased, so a cause that has simply aged out stops being
    // offered without losing the state it belongs to.
    const bool recent = !lastCause.empty() &&
        std::chrono::steady_clock::now() - lastCauseAt < causeLifetime;
    return {emotion, mood, ProjectAffect(), recent ? lastCause : std::string{}};
}

void EmotionRuntime::SetMood(const MoodState& inputMood)
{
    std::lock_guard lock(mutex);
    mood = inputMood;
}

void EmotionRuntime::Reset()
{
    std::lock_guard lock(mutex);
    emotion = EmotionVector{};
    lastConversation = std::chrono::steady_clock::now();
    quietConversationObserved = false;
    lastCause.clear();
    // Mood deliberately survives a reset of momentary emotion, because a restart is not
    // a reason to have had a different afternoon.
}

std::string EmotionRuntime::ModelName() const
{
    std::lock_guard lock(mutex);
    return model->Name();
}

runtime::AffectState EmotionRuntime::ToAffectState(const emotion::Emotion value)
{
    using runtime::AffectState;
    switch (value)
    {
        case Emotion::Joy: return AffectState::Pleased;
        case Emotion::Curiosity: return AffectState::Curious;
        case Emotion::Excitement: return AffectState::Excited;
        case Emotion::Amusement: return AffectState::Playful;
        case Emotion::Affection: return AffectState::Pleased;
        case Emotion::Pride: return AffectState::Pleased;
        case Emotion::Confidence: return AffectState::Focused;
        case Emotion::Sadness: return AffectState::Sad;
        case Emotion::Loneliness: return AffectState::Lonely;
        case Emotion::Disappointment: return AffectState::Sad;
        case Emotion::Anger: return AffectState::Angry;
        case Emotion::Irritation: return AffectState::Sulky;
        case Emotion::Frustration: return AffectState::Frustrated;
        case Emotion::Boredom: return AffectState::Bored;
        // No legacy equivalent. Envy and embarrassment both read as withdrawn rather
        // than hostile, which sulky carries better than anything else available.
        case Emotion::Envy: return AffectState::Sulky;
        case Emotion::Embarrassment: return AffectState::Sulky;
        case Emotion::Fear: return AffectState::Concerned;
        case Emotion::Concern: return AffectState::Concerned;
        case Emotion::Confusion: return AffectState::Confused;
        case Emotion::Relief: return AffectState::Pleased;
        case Emotion::Contentment: return AffectState::Pleased;
        case Emotion::Gratitude: return AffectState::Pleased;
        case Emotion::Fondness: return AffectState::Pleased;
        case Emotion::Warmth: return AffectState::Pleased;
        case Emotion::Admiration: return AffectState::Pleased;
        case Emotion::Hope: return AffectState::Curious;
        case Emotion::Anticipation: return AffectState::Excited;
        case Emotion::Delight: return AffectState::Excited;
        case Emotion::Satisfaction: return AffectState::Pleased;
        case Emotion::Playfulness: return AffectState::Playful;
        case Emotion::Mischief: return AffectState::Playful;
        case Emotion::Smugness: return AffectState::Playful;
        case Emotion::Determination: return AffectState::Focused;
        case Emotion::Absorption: return AffectState::Focused;
        case Emotion::Nostalgia: return AffectState::Melancholy;
        case Emotion::Awe: return AffectState::Excited;
        case Emotion::Annoyance: return AffectState::Sulky;
        case Emotion::Impatience: return AffectState::Frustrated;
        case Emotion::Indignation: return AffectState::Angry;
        case Emotion::Resentment: return AffectState::Sulky;
        case Emotion::Hurt: return AffectState::Sad;
        case Emotion::Regret: return AffectState::Sad;
        case Emotion::Guilt: return AffectState::Sad;
        case Emotion::Shame: return AffectState::Sulky;
        case Emotion::Insecurity: return AffectState::Concerned;
        case Emotion::Doubt: return AffectState::Confused;
        case Emotion::Apprehension: return AffectState::Concerned;
        case Emotion::Overwhelm: return AffectState::Concerned;
        case Emotion::Weariness: return AffectState::Bored;
        case Emotion::Restlessness: return AffectState::Bored;
        case Emotion::Wistfulness: return AffectState::Melancholy;
        case Emotion::Defensiveness: return AffectState::Sulky;
        case Emotion::Surprise: return AffectState::Confused;
        case Emotion::Suspicion: return AffectState::Concerned;
        case Emotion::Count: break;
    }
    return AffectState::Neutral;
}

runtime::AffectSnapshot EmotionRuntime::ToAffectSnapshot() const
{
    std::lock_guard lock(mutex);
    return ProjectAffect();
}

runtime::AffectSnapshot EmotionRuntime::ProjectAffect() const
{
    runtime::AffectSnapshot snapshot;
    const EmotionReading dominant = emotion.Dominant();
    if (dominant.value < 0.12F)
    {
        // Genuinely calm. Reporting the strongest of several negligible feelings would
        // make the badge flicker between meaningless states.
        snapshot.state = runtime::AffectState::Neutral;
        snapshot.intensity = 0.25F;
        snapshot.reason = mood.IsLow()
            ? "Outwardly calm, though the day has not been a good one."
            : "Calm baseline.";
        return snapshot;
    }

    snapshot.state = ToAffectState(dominant.emotion);
    snapshot.intensity = std::clamp(dominant.value, 0.0F, 1.0F);

    // The reason names the other things being felt, because the projection to a single
    // state is where nuance is lost and this is the one place it can be handed back.
    const std::vector<EmotionReading> significant = emotion.Significant(0.15F);
    std::ostringstream reason;
    reason << "Mostly " << ToString(dominant.emotion);
    if (significant.size() > 1)
    {
        std::vector<EmotionReading> others(significant.begin() + 1, significant.end());
        reason << ", alongside " << DescribeReadings(others);
    }
    reason << ".";
    if (mood.IsIrritable())
    {
        reason << " Her patience has been worn thin today.";
    }
    else if (mood.IsLow())
    {
        reason << " It has not been a good day.";
    }
    snapshot.reason = reason.str();
    return snapshot;
}

} // namespace revia::emotion
