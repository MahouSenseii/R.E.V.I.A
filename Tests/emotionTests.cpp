#include "testSupport.h"

#include "Emotion/emotionTypes.h"
#include "Emotion/moodState.h"
#include "Emotion/stimulus.h"

#include <cmath>
#include <iostream>

namespace
{
using revia::tests::Check;
using namespace revia::emotion;

void TestEmotionsAreNotMutuallyExclusive()
{
    // The whole reason this replaces a single enum: being curious about one thing while
    // still annoyed about another has to be representable, or the newer feeling silently
    // erases the older one and she reads as forgetful.
    EmotionVector state;
    state[Emotion::Curiosity] = 0.81F;
    state[Emotion::Amusement] = 0.58F;
    state[Emotion::Irritation] = 0.17F;
    state[Emotion::Confidence] = 0.64F;

    Check(state.Dominant().emotion == Emotion::Curiosity,
        "The strongest emotion was not reported as dominant.");
    Check(std::abs(state.Dominant().value - 0.81F) < 0.001F,
        "The dominant reading lost its strength.");

    const std::vector<EmotionReading> significant = state.Significant(0.15F);
    Check(significant.size() == 4,
        "Simultaneous emotions were collapsed: expected 4, got " +
            std::to_string(significant.size()));
    Check(significant.front().emotion == Emotion::Curiosity &&
        significant.back().emotion == Emotion::Irritation,
        "Significant emotions were not ordered strongest first.");

    // Irritation survives the arrival of a stronger pleasant feeling.
    state[Emotion::Joy] = 0.9F;
    Check(state[Emotion::Irritation] > 0.0F,
        "A new strong emotion overwrote an unrelated existing one.");
}

void TestValenceAndCalmAreHonestAboutNothing()
{
    // An empty state is calm, not miserable. Dividing by a zero total would make silence
    // read as despair, which is the kind of bug that shows up as a permanently sad badge.
    const EmotionVector empty;
    Check(std::abs(empty.Valence()) < 0.0001F,
        "An empty emotional state reported a non-neutral valence.");
    Check(empty.IsCalm(), "An empty emotional state was not calm.");
    Check(empty.Dominant().value == 0.0F,
        "An empty state claimed to have a dominant feeling.");

    EmotionVector pleasant;
    pleasant[Emotion::Joy] = 0.8F;
    Check(pleasant.Valence() > 0.9F, "A purely pleasant state read as mixed.");

    EmotionVector unpleasant;
    unpleasant[Emotion::Sadness] = 0.8F;
    Check(unpleasant.Valence() < -0.9F, "A purely unpleasant state read as mixed.");

    EmotionVector mixed;
    mixed[Emotion::Joy] = 0.5F;
    mixed[Emotion::Sadness] = 0.5F;
    Check(std::abs(mixed.Valence()) < 0.0001F,
        "Equal pleasant and unpleasant feelings did not balance out.");
    Check(!mixed.IsCalm(),
        "A strongly mixed state was mistaken for calm because it averaged to neutral.");
}

void TestEmotionDecaysToNothingRatherThanLingeringForever()
{
    EmotionVector state;
    state[Emotion::Anger] = 0.8F;
    for (int step = 0; step < 40; ++step)
    {
        state.Decay(0.2F);
    }
    Check(state[Emotion::Anger] == 0.0F,
        "A decayed emotion kept an infinitesimal value alive, so 'has she got over it?' "
        "can never be answered yes.");
}

void TestPersistedNamesSurviveEnumReordering()
{
    // Persistence writes names. If it wrote indices, inserting an emotion would silently
    // reinterpret every stored file as a different feeling.
    for (std::size_t index = 0; index < EmotionCount; ++index)
    {
        const auto emotion = static_cast<Emotion>(index);
        const std::string name = ToString(emotion);
        Check(!name.empty() && name != "unknown",
            "An emotion has no persisted name at index " + std::to_string(index));
        Check(EmotionFromString(name) == emotion,
            "Emotion name '" + name + "' did not round-trip.");
    }
    Check(EmotionFromString("not_a_real_emotion") == Emotion::Count,
        "An unknown emotion name was silently mapped onto a real emotion.");
}

void TestMoodIsSlowerThanEmotion()
{
    const MoodController controller;
    MoodState mood;
    EmotionVector angry;
    angry[Emotion::Anger] = 0.9F;
    angry[Emotion::Irritation] = 0.7F;

    const MoodState afterOne = controller.Integrate(mood, angry);
    Check(afterOne.irritability > mood.irritability,
        "A strong angry state did not raise irritability at all.");
    // The load-bearing property: one event nudges, it does not set.
    Check(afterOne.irritability < 0.2F,
        "One angry moment moved mood most of the way, which is emotional whiplash "
        "rather than mood: " + std::to_string(afterOne.irritability));

    MoodState accumulated = mood;
    for (int step = 0; step < 30; ++step)
    {
        accumulated = controller.Integrate(accumulated, angry);
    }
    Check(accumulated.irritability > afterOne.irritability * 3.0F,
        "Repeated anger did not accumulate into an irritable mood.");
    Check(accumulated.valence < -0.05F,
        "Sustained anger left overall mood unaffected.");
}

void TestMoodSaturatesAndReturnsToBaseline()
{
    const MoodController controller;
    EmotionVector miserable;
    miserable[Emotion::Sadness] = 1.0F;
    miserable[Emotion::Disappointment] = 1.0F;

    MoodState mood;
    for (int step = 0; step < 400; ++step)
    {
        mood = controller.Integrate(mood, miserable);
    }
    // Saturation: relentless bad news deepens mood with diminishing returns instead of
    // pinning it at the floor, so there is always somewhere worse for a real crisis.
    Check(mood.valence > -1.0F,
        "Mood ran away to the floor instead of saturating.");
    Check(mood.valence < -0.3F,
        "Sustained misery barely moved mood: " + std::to_string(mood.valence));

    // And with nothing happening it comes back, without snapping.
    const float lowest = mood.valence;
    MoodState settling = mood;
    settling = controller.Settle(settling);
    Check(settling.valence > lowest,
        "Mood did not recover at all during quiet time.");
    Check(settling.valence < lowest + 0.15F,
        "Mood snapped back toward baseline in one step instead of easing.");

    for (int step = 0; step < 500; ++step)
    {
        settling = controller.Settle(settling);
    }
    Check(std::abs(settling.valence - settling.baselineValence) < 0.05F,
        "Mood never returned to its baseline given enough quiet.");
    Check(settling.irritability < 0.01F,
        "Irritability persisted with nothing renewing it.");
}

void TestMoodChangesHowTheNextThingLands()
{
    // This is the feedback loop that makes a bad day mean something: mood does not decide
    // what is felt, it decides how hard it lands.
    const MoodController controller;

    MoodState calm;
    MoodState irritable;
    irritable.irritability = 0.9F;

    const float calmGain = controller.AppraisalGain(calm, Emotion::Irritation);
    const float irritableGain = controller.AppraisalGain(irritable, Emotion::Irritation);
    Check(irritableGain > calmGain * 1.2F,
        "An already-irritable mood did not shorten the fuse.");

    MoodState good;
    good.valence = 0.8F;
    Check(controller.AppraisalGain(good, Emotion::Irritation) < calmGain,
        "A good mood did not absorb a small annoyance.");
    Check(controller.AppraisalGain(good, Emotion::Joy) > calmGain,
        "A good mood did not make delight easier to reach.");

    MoodState low;
    low.valence = -0.8F;
    Check(controller.AppraisalGain(low, Emotion::Joy) < calmGain,
        "Delight was as easy to reach while worn down as while content.");

    // Bounded in both directions, so mood colours a reaction and never replaces it.
    for (const MoodState& state : {calm, irritable, good, low})
    {
        for (std::size_t index = 0; index < EmotionCount; ++index)
        {
            const float gain = controller.AppraisalGain(state, static_cast<Emotion>(index));
            Check(gain >= 0.4F && gain <= 1.8F,
                "An appraisal gain escaped its bounds: " + std::to_string(gain));
        }
    }
}

void TestStimulusGatesOnImportanceAndCertainty()
{
    Stimulus trivial;
    trivial.importance = 0.05F;
    Check(!trivial.IsMeaningful(),
        "A trivial event was considered worth feeling.");

    Stimulus important;
    important.importance = 0.8F;
    Check(important.IsMeaningful(), "An important certain event was discarded.");

    // Certainty gates alongside importance. A big event the runtime is unsure actually
    // happened must not produce a confident feeling.
    Stimulus unsure = important;
    unsure.certainty = 0.1F;
    Check(!unsure.IsMeaningful(),
        "A barely-believed event produced a feeling as strong as a confirmed one.");

    for (const StimulusSource source : {
        StimulusSource::Conversation, StimulusSource::Relationship,
        StimulusSource::Perception, StimulusSource::Memory, StimulusSource::Goal,
        StimulusSource::Action, StimulusSource::Research, StimulusSource::Environment,
        StimulusSource::Internal})
    {
        Check(StimulusSourceFromString(ToString(source)) == source,
            "Stimulus source '" + ToString(source) + "' did not round-trip.");
    }
}
}

void RunEmotionTests()
{
    TestEmotionsAreNotMutuallyExclusive();
    TestValenceAndCalmAreHonestAboutNothing();
    TestEmotionDecaysToNothingRatherThanLingeringForever();
    TestPersistedNamesSurviveEnumReordering();
    TestMoodIsSlowerThanEmotion();
    TestMoodSaturatesAndReturnsToBaseline();
    TestMoodChangesHowTheNextThingLands();
    TestStimulusGatesOnImportanceAndCertainty();
    std::cout << "Emotions coexist, mood lags and saturates, and stimuli gate on "
                 "importance and certainty.\n";
}
