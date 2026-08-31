#include "testSupport.h"

#include "Emotion/stimulusBuilder.h"
#include "Identity/developmentEngine.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace
{
using revia::tests::Check;
using namespace revia::identity;

std::size_t ApplyMany(
    DevelopmentEngine& engine,
    DevelopmentState& development,
    const DevelopmentEvidence& evidence,
    const int times)
{
    std::size_t applied = 0;
    for (int index = 0; index < times; ++index)
    {
        if (const std::optional<DevelopmentChange> change = engine.Observe(evidence))
        {
            development = DevelopmentEngine::Apply(development, *change, engine.Limits());
            ++applied;
        }
    }
    return applied;
}

void TestOneConversationCannotRewriteHer()
{
    // The property the whole subsystem exists for. If a handful of messages could move a
    // trait, "you're actually very obedient" would be self-fulfilling.
    DevelopmentEngine engine;
    DevelopmentState development;
    const float before = development.Current(Trait::Confidence);

    const DevelopmentEvidence evidence{
        Trait::Confidence, true, "problems she took on herself kept working out"};
    for (int index = 0; index < 3; ++index)
    {
        Check(!engine.Observe(evidence).has_value(),
            "A personality change was applied after only " + std::to_string(index + 1) +
                " observation(s).");
    }
    Check(std::abs(development.Current(Trait::Confidence) - before) < 0.0001F,
        "Personality moved before any change was applied.");

    // And when it finally does move, it moves barely at all.
    const std::optional<DevelopmentChange> change = engine.Observe(evidence);
    Check(change.has_value(), "Sustained consistent evidence never produced a change.");
    Check(std::abs(change->delta) <= engine.Limits().maximumStep + 0.0001F,
        "A single applied change exceeded the step ceiling.");
    Check(change->evidenceCount >= 4,
        "The applied change did not record how much evidence backed it.");
    Check(!change->reason.empty(), "An applied change carried no explanation.");
    Check(!change->recordedAt.empty(), "An applied change carried no timestamp.");
}

void TestContradictingEvidenceCancelsRatherThanRatchets()
{
    // Without this, development is a ratchet: a trait only ever moves in whichever
    // direction it happened to move first, and a habit she grew out of keeps pulling.
    DevelopmentEngine engine;
    const DevelopmentEvidence up{Trait::Impulsiveness, true, "moving quickly worked"};
    const DevelopmentEvidence down{Trait::Impulsiveness, false, "rushing went wrong"};

    engine.Observe(up);
    engine.Observe(up);
    engine.Observe(down);
    engine.Observe(down);
    Check(std::abs(engine.PendingEvidence(Trait::Impulsiveness)) < 0.0001F,
        "Equal evidence in both directions did not cancel out.");

    // And sustained evidence the other way genuinely reverses it.
    DevelopmentState development;
    const float start = development.Current(Trait::Impulsiveness);
    ApplyMany(engine, development, down, 40);
    const float lowered = development.Current(Trait::Impulsiveness);
    Check(lowered < start, "Repeated failed impulsive attempts did not reduce impulsiveness.");

    DevelopmentEngine reversing;
    ApplyMany(reversing, development, up, 60);
    Check(development.Current(Trait::Impulsiveness) > lowered,
        "Development could not reverse when the evidence changed direction.");
}

void TestDriftIsCappedSoSheStaysRecognisable()
{
    DevelopmentEngine engine;
    DevelopmentState development;
    const DevelopmentEvidence evidence{Trait::Caution, true, "caught out by not checking"};

    // Far more evidence than could ever accumulate in practice.
    ApplyMany(engine, development, evidence, 4000);
    Check(development.Drift(Trait::Caution) <= engine.Limits().maximumDrift + 0.0001F,
        "Lifetime drift escaped its cap: " +
            std::to_string(development.Drift(Trait::Caution)));
    Check(development.Drift(Trait::Caution) > 0.1F,
        "Sustained evidence produced almost no change at all.");

    // The original personality is untouched, so how she changed remains answerable by
    // subtraction rather than by guesswork.
    Check(std::abs(development.base[Trait::Caution] -
        ChildlikeBaseline()[Trait::Caution]) < 0.0001F,
        "Development overwrote the original personality.");
}

void TestDevelopmentHasNoPreferredDirection()
{
    // Growth that could only make her calmer and more agreeable would just be sanding
    // her down into a generic assistant.
    const TurnObservation impulsiveAndWorked{true, false, true, false, false, false};
    const std::vector<DevelopmentEvidence> paidOff =
        ReadDevelopmentEvidence(impulsiveAndWorked);
    const bool raisesImpulsiveness = std::any_of(paidOff.begin(), paidOff.end(),
        [](const DevelopmentEvidence& evidence)
        {
            return evidence.trait == Trait::Impulsiveness && evidence.increases;
        });
    Check(raisesImpulsiveness,
        "Impulsiveness that paid off could not push her toward more of it, so caution "
        "is a one-way slide.");

    const TurnObservation impulsiveAndFailed{false, false, true, false, false, false};
    const std::vector<DevelopmentEvidence> backfired =
        ReadDevelopmentEvidence(impulsiveAndFailed);
    const bool lowersImpulsiveness = std::any_of(backfired.begin(), backfired.end(),
        [](const DevelopmentEvidence& evidence)
        {
            return evidence.trait == Trait::Impulsiveness && !evidence.increases;
        });
    Check(lowersImpulsiveness, "Failed impulsive attempts did not reduce impulsiveness.");

    // An ordinary uneventful turn observes nothing at all.
    const TurnObservation nothingHappened{true, false, false, false, false, false};
    Check(ReadDevelopmentEvidence(nothingHappened).empty(),
        "An uneventful turn still produced development evidence.");
}

void TestStimuliCarryCausationFromRealOutcomes()
{
    using namespace revia::emotion;

    // A goal she ran is hers; policy refusing it is not. This is what separates
    // frustration from concern downstream.
    const Stimulus failed = BuildGoalStimulus(false, false, false, 6, 1, "it did not finish");
    Check(failed.selfCaused && failed.failure > 0.5F,
        "A failed goal was not recorded as her own failure.");
    const Stimulus blocked = BuildGoalStimulus(false, false, true, 2, 0, "policy refused it");
    Check(!blocked.selfCaused,
        "A goal stopped by policy was recorded as her own failure.");

    const Stimulus smooth = BuildGoalStimulus(true, false, false, 4, 0, "done");
    const Stimulus hardWon = BuildGoalStimulus(true, false, false, 4, 3, "done eventually");
    Check(hardWon.novelty > smooth.novelty,
        "A hard-won success was indistinguishable from a routine one.");

    // A hostile remark is the user's doing; being corrected is hers.
    const ConversationSignals hostile =
        ReadConversationSignals("you're useless", "I see.", true);
    const Stimulus jab = BuildConversationStimulus("someone", hostile);
    Check(jab.userCaused && !jab.selfCaused && jab.valence < -0.5F,
        "A hostile remark was not attributed to the person who made it.");

    const ConversationSignals corrected =
        ReadConversationSignals("no, I already said that", "Sorry.", true);
    const Stimulus miss = BuildConversationStimulus("someone", corrected);
    Check(miss.selfCaused,
        "Having to be corrected was not recorded as her own miss.");

    // An ordinary message is flat. Giving every turn a valence is how a companion ends
    // up visibly reacting to "ok".
    const ConversationSignals ordinary =
        ReadConversationSignals("can you check the log file", "Sure.", true);
    Check(std::abs(BuildConversationStimulus("someone", ordinary).valence) < 0.0001F,
        "An ordinary message carried an emotional charge.");
}
}

void RunDevelopmentTests()
{
    TestOneConversationCannotRewriteHer();
    TestContradictingEvidenceCancelsRatherThanRatchets();
    TestDriftIsCappedSoSheStaysRecognisable();
    TestDevelopmentHasNoPreferredDirection();
    TestStimuliCarryCausationFromRealOutcomes();
    std::cout << "Personality changes slowly, reversibly, within bounds, and in no "
                 "preferred direction.\n";
}
