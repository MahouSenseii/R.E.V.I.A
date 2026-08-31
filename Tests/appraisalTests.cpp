#include "testSupport.h"

#include "Emotion/emotionModel.h"
#include "Emotion/emotionRuntime.h"

#include <cmath>
#include <iostream>

namespace
{
using revia::tests::Check;
using namespace revia::emotion;
using revia::identity::DevelopmentState;
using revia::identity::RelationshipState;
using revia::identity::Trait;

Stimulus GoalOutcome(const bool succeeded, const bool selfCaused, const float importance = 0.7F)
{
    Stimulus stimulus;
    stimulus.source = StimulusSource::Goal;
    stimulus.eventType = succeeded ? "goal_succeeded" : "goal_failed";
    stimulus.description = succeeded ? "the run finished" : "the run did not finish";
    stimulus.importance = importance;
    stimulus.success = succeeded ? 0.9F : 0.0F;
    stimulus.failure = succeeded ? 0.0F : 0.9F;
    stimulus.valence = succeeded ? 0.6F : -0.6F;
    stimulus.selfCaused = selfCaused;
    return stimulus;
}

Stimulus HostileRemark()
{
    Stimulus stimulus;
    stimulus.source = StimulusSource::Conversation;
    stimulus.eventType = "hostile_remark";
    stimulus.subjectId = "quentin";
    stimulus.description = "a sharp remark";
    stimulus.valence = -0.8F;
    stimulus.importance = 0.6F;
    stimulus.userCaused = true;
    return stimulus;
}

RelationshipState CloseFriend()
{
    RelationshipState relationship;
    relationship.entityId = "quentin";
    relationship.familiarity = 0.9F;
    relationship.affinity = 0.7F;
    relationship.trust = 0.85F;
    relationship.irritation = 0.05F;
    return relationship;
}

RelationshipState Stranger()
{
    RelationshipState relationship;
    relationship.entityId = "someone";
    relationship.familiarity = 0.05F;
    return relationship;
}

void TestSameEventDiffersByWhoCausedIt()
{
    // The single most important distinction appraisal draws: her approach failing is
    // frustrating, something breaking underneath her is worrying.
    const RuleEmotionModel model;
    const DevelopmentState development;
    const EmotionVector calm;
    const MoodState mood;

    const Stimulus hers = GoalOutcome(false, true);
    const Stimulus theirs = GoalOutcome(false, false);

    const EmotionVector fromHers = model.Evaluate(
        hers, BuildAppraisalContext(hers, development, mood, calm));
    const EmotionVector fromTheirs = model.Evaluate(
        theirs, BuildAppraisalContext(theirs, development, mood, calm));

    Check(fromHers[Emotion::Frustration] > fromTheirs[Emotion::Frustration] * 2.0F,
        "A self-caused failure was no more frustrating than an external one.");
    Check(fromTheirs[Emotion::Concern] > fromHers[Emotion::Concern],
        "An externally caused failure did not produce more concern.");
}

void TestSuccessNeedsOwnershipToBecomePride()
{
    const RuleEmotionModel model;
    const DevelopmentState development;
    const EmotionVector calm;
    const MoodState mood;

    const Stimulus hers = GoalOutcome(true, true);
    const Stimulus theirs = GoalOutcome(true, false);

    const EmotionVector fromHers = model.Evaluate(
        hers, BuildAppraisalContext(hers, development, mood, calm));
    const EmotionVector fromTheirs = model.Evaluate(
        theirs, BuildAppraisalContext(theirs, development, mood, calm));

    Check(fromHers[Emotion::Pride] > 0.05F,
        "Succeeding at something she chose produced no pride.");
    Check(fromTheirs[Emotion::Pride] < 0.01F,
        "She was proud of an outcome she had no hand in.");
    Check(fromTheirs[Emotion::Joy] > 0.05F,
        "A good outcome she did not cause was not even pleasant.");
}

void TestRelationshipDecidesWhetherAJabIsAJoke()
{
    const RuleEmotionModel model;
    const DevelopmentState development;
    const EmotionVector calm;
    const MoodState mood;
    const Stimulus remark = HostileRemark();

    const RelationshipState friendly = CloseFriend();
    const EmotionVector fromFriend = model.Evaluate(
        remark, BuildAppraisalContext(remark, development, mood, calm, &friendly));

    const RelationshipState unknown = Stranger();
    const EmotionVector fromStranger = model.Evaluate(
        remark, BuildAppraisalContext(remark, development, mood, calm, &unknown));

    Check(fromFriend[Emotion::Amusement] > fromFriend[Emotion::Anger],
        "A jab from a close friend was read as an attack rather than teasing.");
    Check(fromStranger[Emotion::Anger] > fromFriend[Emotion::Anger] * 2.0F,
        "A stranger got the same benefit of the doubt as a close friend.");
    Check(fromStranger[Emotion::Amusement] < 0.01F,
        "Hostility from a stranger was found amusing.");

    // Closeness is not armour. From someone she trusts, when the relationship is already
    // strained, the same words hurt instead of amusing.
    RelationshipState strained = CloseFriend();
    strained.irritation = 0.8F;
    const EmotionVector fromStrained = model.Evaluate(
        remark, BuildAppraisalContext(remark, development, mood, calm, &strained));
    Check(fromStrained[Emotion::Sadness] > fromStrained[Emotion::Amusement],
        "A sharp remark from someone close on a bad day was still taken as a joke.");
}

void TestPersonalityScalesButDoesNotCreateFeeling()
{
    // Personality must remain a separate influence. It may amplify or damp what an event
    // produced; it must never manufacture an emotion the event did not cause.
    const RuleEmotionModel model;
    const MoodState mood;
    const EmotionVector calm;

    Stimulus discovery;
    discovery.source = StimulusSource::Research;
    discovery.eventType = "discovery";
    discovery.description = "something unexpected turned up";
    discovery.importance = 0.6F;
    discovery.novelty = 0.9F;

    DevelopmentState incurious;
    incurious.base[Trait::Curiosity] = 0.1F;
    DevelopmentState fascinated;
    fascinated.base[Trait::Curiosity] = 0.95F;

    const EmotionVector low = model.Evaluate(
        discovery, BuildAppraisalContext(discovery, incurious, mood, calm));
    const EmotionVector high = model.Evaluate(
        discovery, BuildAppraisalContext(discovery, fascinated, mood, calm));
    Check(high[Emotion::Curiosity] > low[Emotion::Curiosity] * 1.5F,
        "A curious personality was no more interested than an incurious one.");

    // And with nothing happening, no personality produces a feeling from nowhere.
    Stimulus nothing;
    nothing.importance = 0.0F;
    const EmotionVector fromNothing = model.Evaluate(
        nothing, BuildAppraisalContext(nothing, fascinated, mood, calm));
    Check(fromNothing.TotalIntensity() < 0.0001F,
        "Personality manufactured an emotion with no event behind it.");

    // The raw appraisal is available separately, so the influence of character can be
    // measured rather than asserted.
    const EmotionVector raw = RuleEmotionModel::RawResponse(
        discovery, BuildAppraisalContext(discovery, fascinated, mood, calm));
    Check(raw[Emotion::Curiosity] > 0.0F && raw[Emotion::Curiosity] != high[Emotion::Curiosity],
        "The raw appraisal was indistinguishable from the personality-scaled result.");
}

void TestMoodChangesHowTheNextEventLands()
{
    const RuleEmotionModel model;
    const DevelopmentState development;
    const EmotionVector calm;

    Stimulus annoyance;
    annoyance.source = StimulusSource::Conversation;
    annoyance.eventType = "minor_annoyance";
    annoyance.valence = -0.35F;
    annoyance.importance = 0.4F;

    MoodState fresh;
    MoodState wornDown;
    wornDown.irritability = 0.9F;
    wornDown.valence = -0.5F;

    const EmotionVector onGoodDay = model.Evaluate(
        annoyance, BuildAppraisalContext(annoyance, development, fresh, calm));
    const EmotionVector onBadDay = model.Evaluate(
        annoyance, BuildAppraisalContext(annoyance, development, wornDown, calm));

    Check(onBadDay[Emotion::Irritation] > onGoodDay[Emotion::Irritation] * 1.2F,
        "The same small annoyance landed identically on a good and a bad day.");
}

void TestRuntimeAccumulatesAndSettles()
{
    EmotionRuntime runtime;
    const DevelopmentState development;

    Check(runtime.Emotion().IsCalm(), "A fresh emotion runtime did not start calm.");

    // A trivial event is not worth feeling, and says so by returning nothing rather than
    // by returning an unchanged state.
    Stimulus trivial = GoalOutcome(true, true, 0.05F);
    Check(!runtime.Observe(trivial, development).has_value(),
        "A trivial event still produced an appraisal.");
    Check(runtime.Emotion().IsCalm(), "A trivial event moved the emotional state.");

    const std::optional<AppraisalOutcome> outcome =
        runtime.Observe(GoalOutcome(false, true), development);
    Check(outcome.has_value(), "An important self-caused failure produced no feeling.");
    Check(outcome->changed && outcome->modelName == "rule-v1",
        "The appraisal did not record which model produced it.");
    Check(!outcome->explanation.empty(),
        "An appraisal produced no explanation, so the feeling cannot be traced.");
    Check(outcome->delta[Emotion::Frustration] > 0.0F,
        "A failed goal produced no frustration.");
    Check(runtime.Emotion()[Emotion::Frustration] > 0.0F,
        "The appraisal did not reach the current emotional state.");

    // Emotions coexist: a discovery does not erase the frustration.
    const float frustrationBefore = runtime.Emotion()[Emotion::Frustration];
    Stimulus discovery;
    discovery.source = StimulusSource::Research;
    discovery.importance = 0.7F;
    discovery.novelty = 0.9F;
    discovery.description = "an unexpected result";
    Check(runtime.Observe(discovery, development).has_value(),
        "A novel discovery produced no feeling.");
    Check(runtime.Emotion()[Emotion::Curiosity] > 0.0F,
        "A discovery did not make her curious.");
    Check(runtime.Emotion()[Emotion::Frustration] >= frustrationBefore * 0.99F,
        "A new feeling erased an unrelated existing one.");

    // Settling fades emotion but leaves mood carrying what happened.
    const MoodState moodBefore = runtime.Mood();
    for (int step = 0; step < 60; ++step)
    {
        runtime.Settle(0.15F);
    }
    Check(runtime.Emotion().IsCalm(),
        "Emotion never faded despite a long quiet stretch.");
    Check(runtime.Mood().valence <= moodBefore.valence + 0.35F,
        "Mood snapped back to neutral the moment the feelings faded.");
}

void TestLegacyAffectBridgeStaysHonest()
{
    // The projection to the old single-state shape is lossy, and the reason line is the
    // one place that nuance can be handed back. What it must never do is misreport calm.
    EmotionRuntime emotions;
    const revia::runtime::AffectSnapshot idle = emotions.ToAffectSnapshot();
    Check(idle.state == revia::runtime::AffectState::Neutral,
        "A calm emotional state did not project to Neutral.");

    const DevelopmentState development;
    Check(emotions.Observe(GoalOutcome(false, true), development).has_value(),
        "The setup stimulus produced no feeling.");

    const revia::runtime::AffectSnapshot after = emotions.ToAffectSnapshot();
    Check(after.state != revia::runtime::AffectState::Neutral,
        "A real feeling still projected to Neutral.");
    Check(after.intensity > 0.0F && after.intensity <= 1.0F,
        "The bridged intensity escaped its range.");
    Check(!after.reason.empty(), "The bridged snapshot carried no reason.");

    // Every emotion has to resolve to some legacy state, or the badge silently blanks
    // whenever an unmapped feeling happens to be dominant.
    for (std::size_t index = 0; index < EmotionCount; ++index)
    {
        const auto value = static_cast<Emotion>(index);
        Check(EmotionRuntime::ToAffectState(value) != revia::runtime::AffectState::Neutral,
            "Emotion '" + ToString(value) + "' has no legacy affect mapping.");
    }
}

void TestMemoriesTemperSurprise()
{
    // A familiar disappointment should not land like a fresh shock every time.
    const DevelopmentState development;
    const MoodState mood;
    const EmotionVector calm;

    Stimulus failure = GoalOutcome(false, true);
    failure.novelty = 0.7F;

    const AppraisalContext withoutHistory =
        BuildAppraisalContext(failure, development, mood, calm);

    std::vector<RelevantMemory> history;
    for (int index = 0; index < 3; ++index)
    {
        RelevantMemory memory;
        memory.summary = "this approach failed before";
        memory.importance = 0.8F;
        memory.similarity = 0.9F;
        memory.pastValence = -0.8F;
        history.push_back(memory);
    }
    const AppraisalContext withHistory = BuildAppraisalContext(
        failure, development, mood, calm, nullptr, history);

    Check(withHistory.expectedness > withoutHistory.expectedness,
        "Repeated past failures did not make this one more expected.");
    Check(withHistory.novelty < withoutHistory.novelty,
        "A familiar failure was still treated as novel.");

    // Bounded: the whole database must never reach one appraisal.
    std::vector<RelevantMemory> flood(50);
    for (RelevantMemory& memory : flood)
    {
        memory.importance = 0.9F;
        memory.similarity = 0.9F;
    }
    const AppraisalContext bounded = BuildAppraisalContext(
        failure, development, mood, calm, nullptr, flood);
    Check(bounded.memories.size() <= AppraisalContext::maximumMemories,
        "An appraisal accepted an unbounded number of memories.");
}
}

void RunAppraisalTests()
{
    TestSameEventDiffersByWhoCausedIt();
    TestSuccessNeedsOwnershipToBecomePride();
    TestRelationshipDecidesWhetherAJabIsAJoke();
    TestPersonalityScalesButDoesNotCreateFeeling();
    TestMoodChangesHowTheNextEventLands();
    TestRuntimeAccumulatesAndSettles();
    TestLegacyAffectBridgeStaysHonest();
    TestMemoriesTemperSurprise();
    std::cout << "Appraisal weighs cause, relationship, personality and mood, and the "
                 "legacy affect bridge stays honest.\n";
}
