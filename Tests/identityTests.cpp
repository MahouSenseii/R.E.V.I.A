#include "testSupport.h"

#include "Identity/developmentState.h"
#include "Identity/identityStore.h"
#include "Identity/relationshipState.h"

#include <cmath>
#include <fstream>
#include <iostream>

namespace
{
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
using namespace revia::identity;

RelationshipEvent WarmExchange(const float importance = 0.6F)
{
    RelationshipEvent event;
    event.entityId = "quentin";
    event.positiveInteraction = 0.8F;
    event.cooperation = 0.6F;
    event.trustEvidence = 0.5F;
    event.importance = importance;
    return event;
}

RelationshipEvent HostileExchange(const float importance = 0.6F)
{
    RelationshipEvent event;
    event.entityId = "stranger";
    event.negativeInteraction = 0.9F;
    event.conflict = 0.7F;
    event.disrespectEvidence = 0.6F;
    event.importance = importance;
    return event;
}

void TestBaseAndDeltaStaySeparate()
{
    // Keeping them apart is what makes "how has she changed?" answerable by subtraction
    // rather than by guesswork, and what makes a change reversible.
    DevelopmentState development;
    const float startingImpulsiveness = development.Current(Trait::Impulsiveness);

    development.delta[Trait::Impulsiveness] = -0.12F;
    Check(std::abs(development.base[Trait::Impulsiveness] -
        ChildlikeBaseline()[Trait::Impulsiveness]) < 0.0001F,
        "Applying a development delta overwrote the original personality.");
    Check(development.Current(Trait::Impulsiveness) < startingImpulsiveness,
        "A negative delta did not lower the current trait.");
    Check(std::abs(development.Drift(Trait::Impulsiveness) + 0.12F) < 0.0001F,
        "Drift did not report the accumulated change.");

    // Current is clamped even when an accumulated offset would push past the scale.
    development.delta[Trait::Curiosity] = 5.0F;
    Check(development.Current(Trait::Curiosity) <= 1.0F,
        "A large delta pushed a trait outside its own scale.");
}

void TestSheStartsChildlikeButNotFixed()
{
    const TraitVector baseline = ChildlikeBaseline();
    // Her documented starting temperament, as numbers the runtime can act on.
    Check(baseline[Trait::Curiosity] > 0.7F, "Revia did not start curious.");
    Check(baseline[Trait::Playfulness] > 0.7F, "Revia did not start playful.");
    Check(baseline[Trait::Impulsiveness] > 0.6F, "Revia did not start impulsive.");
    Check(baseline[Trait::EmotionalExpressiveness] > 0.7F,
        "Revia did not start emotionally expressive.");
    Check(baseline[Trait::Patience] < 0.45F, "Revia started unusually patient.");
    Check(baseline[Trait::EmotionalRegulation] < 0.45F,
        "Revia started unusually well regulated.");
    Check(baseline[Trait::Maturity] < 0.45F, "Revia did not start young.");

    // Every trait must be movable in both directions. Development that could only make
    // her calmer and more agreeable would just be sanding her down into an assistant.
    DevelopmentState development;
    for (std::size_t index = 0; index < TraitCount; ++index)
    {
        const auto trait = static_cast<Trait>(index);
        development.delta[trait] = 0.1F;
        const float raised = development.Current(trait);
        development.delta[trait] = -0.1F;
        const float lowered = development.Current(trait);
        Check(raised > lowered,
            "Trait '" + ToString(trait) + "' cannot move in both directions.");
        development.delta[trait] = 0.0F;
    }
}

void TestDriftIsExplainedInPlainLanguage()
{
    DevelopmentState development;
    Check(development.DescribeDrift().empty(),
        "An unchanged personality claimed to have drifted.");

    development.delta[Trait::Impulsiveness] = -0.18F;
    development.delta[Trait::Caution] = 0.11F;
    const std::string description = development.DescribeDrift();
    Check(description.find("less impulsive") != std::string::npos,
        "Drift did not describe the direction of the largest change: " + description);
    Check(description.find("more cautious") != std::string::npos,
        "Drift omitted a second meaningful change: " + description);

    // Bounded to the largest few, because this reaches a prompt and sixteen small
    // drifts is noise rather than self-knowledge.
    for (std::size_t index = 0; index < TraitCount; ++index)
    {
        development.delta[static_cast<Trait>(index)] = 0.2F;
    }
    const std::string many = development.DescribeDrift();
    Check(many.find("and") != std::string::npos && many.size() < 220,
        "A fully drifted personality produced an unbounded description.");
}

void TestRelationshipsAreIndependentAndNotAssumedWarm()
{
    // Nothing assumes she likes the primary user. Affinity starts neutral and is earned.
    RelationshipState fresh;
    Check(std::abs(fresh.affinity) < 0.0001F,
        "A new relationship started with a non-neutral opinion.");
    Check(fresh.familiarity == 0.0F, "A stranger started already familiar.");

    RelationshipState friendly;
    friendly.entityId = "quentin";
    for (int exchange = 0; exchange < 200; ++exchange)
    {
        friendly = ApplyRelationshipEvent(friendly, WarmExchange());
    }

    RelationshipState hostile;
    hostile.entityId = "stranger";
    for (int exchange = 0; exchange < 200; ++exchange)
    {
        hostile = ApplyRelationshipEvent(hostile, HostileExchange());
    }

    Check(friendly.affinity > 0.4F,
        "Sustained warmth did not build affinity: " + std::to_string(friendly.affinity));
    Check(hostile.affinity < -0.2F,
        "Sustained hostility did not lower affinity: " + std::to_string(hostile.affinity));
    Check(friendly.trust > hostile.trust,
        "Trust did not distinguish between the two people.");
    Check(hostile.resentment > friendly.resentment,
        "Grievance accrued equally for cooperation and disrespect.");
    Check(friendly.ReadsAsTeasing() && !hostile.ReadsAsTeasing(),
        "A jab would be read the same way from a friend and from a hostile stranger.");
}

void TestFrictionIsNotAVerdict()
{
    // She can be furious with someone she trusts. Collapsing emotion into relationship
    // would make every argument a betrayal.
    RelationshipState close;
    close.entityId = "quentin";
    for (int exchange = 0; exchange < 200; ++exchange)
    {
        close = ApplyRelationshipEvent(close, WarmExchange());
    }
    const float trustBefore = close.trust;
    const float affinityBefore = close.affinity;

    RelationshipEvent argument;
    argument.entityId = "quentin";
    argument.negativeInteraction = 0.7F;
    argument.conflict = 0.8F;
    argument.importance = 0.7F;
    close = ApplyRelationshipEvent(close, argument);

    Check(close.irritation > 0.0F, "An argument produced no friction at all.");
    Check(close.trust > trustBefore - 0.05F,
        "One argument materially damaged trust that took two hundred exchanges to build.");
    Check(close.affinity > affinityBefore - 0.1F,
        "One argument undid accumulated affinity.");
    Check(close.resentment < 0.05F,
        "Ordinary conflict accrued as lasting grievance, so every disagreement becomes "
        "permanent.");

    // Friction cools with time; grievance does not, at anything like the same rate.
    RelationshipState settling = close;
    settling.resentment = 0.5F;
    const float resentmentBefore = settling.resentment;
    for (int step = 0; step < 5; ++step)
    {
        settling = SettleRelationship(settling);
    }
    Check(settling.irritation == 0.0F, "Current friction did not cool with time.");
    Check(settling.resentment > resentmentBefore - 0.1F,
        "Grievance evaporated as quickly as momentary irritation.");
    Check(std::abs(settling.familiarity - close.familiarity) < 0.0001F,
        "Time apart erased how well she knows someone.");
}

void TestRelationshipMovementIsBoundedPerExchange()
{
    // One message must not be able to rewrite a relationship, or "we're best friends now"
    // becomes true by saying it.
    RelationshipState state;
    state.entityId = "someone";
    RelationshipEvent enormous;
    enormous.entityId = "someone";
    enormous.positiveInteraction = 1.0F;
    enormous.trustEvidence = 1.0F;
    enormous.cooperation = 1.0F;
    enormous.importance = 1.0F;
    enormous.confidence = 1.0F;

    const RelationshipState after = ApplyRelationshipEvent(state, enormous);
    Check(after.affinity <= 0.11F,
        "A single exchange moved affinity too far: " + std::to_string(after.affinity));
    Check(after.trust - state.trust <= 0.06F,
        "A single exchange moved trust too far.");

    // An unimportant or barely-believed exchange should move almost nothing.
    RelationshipEvent uncertain = enormous;
    uncertain.confidence = 0.0F;
    const RelationshipState unchanged = ApplyRelationshipEvent(state, uncertain);
    Check(std::abs(unchanged.affinity - state.affinity) < 0.0001F,
        "An exchange the runtime did not believe still moved the relationship.");
    Check(unchanged.interactionCount == state.interactionCount,
        "A discarded exchange still counted as an interaction.");
}

void TestIdentitySurvivesRestartAndRefusesToGuess()
{
    ScopedTestDirectory directory;
    const std::filesystem::path path = directory.root / "identity.json";

    // A missing file is a first run, not a failure.
    {
        const IdentityStore store(path);
        IdentitySnapshot snapshot;
        std::string error;
        Check(store.Load(snapshot, error),
            "A first run was treated as a load failure: " + error);
        Check(std::abs(snapshot.development.base[Trait::Curiosity] -
            ChildlikeBaseline()[Trait::Curiosity]) < 0.0001F,
            "A first run did not start from the childlike baseline.");
        Check(!store.StoredVersion().has_value(),
            "A missing identity file reported a schema version.");
    }

    IdentitySnapshot written;
    written.development.delta[Trait::Impulsiveness] = -0.14F;
    written.development.delta[Trait::Caution] = 0.09F;
    written.mood.valence = -0.31F;
    written.mood.irritability = 0.42F;
    RelationshipState quentin;
    quentin.entityId = "quentin";
    quentin.displayName = "Quentin";
    quentin.familiarity = 0.94F;
    quentin.affinity = 0.82F;
    quentin.trust = 0.89F;
    quentin.irritation = 0.18F;
    written.relationships.emplace("quentin", quentin);
    DevelopmentChange change;
    change.trait = Trait::Impulsiveness;
    change.delta = -0.14F;
    change.reason = "repeated impulsive attempts that did not work out";
    change.evidenceCount = 11;
    written.developmentHistory.push_back(change);

    {
        const IdentityStore store(path);
        std::string error;
        Check(store.Save(written, error), "Identity could not be saved: " + error);
        Check(store.StoredVersion() == IdentitySchemaVersion,
            "The saved identity did not record its schema version.");
    }

    // A separate instance, because surviving a restart is the entire feature.
    {
        const IdentityStore reopened(path);
        IdentitySnapshot restored;
        std::string error;
        Check(reopened.Load(restored, error), "Identity could not be reloaded: " + error);

        Check(std::abs(restored.development.delta[Trait::Impulsiveness] + 0.14F) < 0.0001F,
            "Learned development did not survive a restart.");
        Check(std::abs(restored.development.base[Trait::Curiosity] -
            ChildlikeBaseline()[Trait::Curiosity]) < 0.0001F,
            "The original personality was not preserved alongside the learned delta.");
        Check(std::abs(restored.mood.irritability - 0.42F) < 0.0001F,
            "Mood did not survive a restart.");

        const auto found = restored.relationships.find("quentin");
        Check(found != restored.relationships.end(),
            "A stored relationship was lost across a restart.");
        Check(std::abs(found->second.trust - 0.89F) < 0.0001F &&
            found->second.displayName == "Quentin",
            "A restored relationship lost its detail.");
        Check(restored.developmentHistory.size() == 1 &&
            restored.developmentHistory.front().evidenceCount == 11,
            "The explanation for a development change was not persisted.");
    }

    // A corrupt file is refused, never silently replaced. Overwriting it would delete
    // everything she had become and the only symptom would be that she felt different.
    {
        std::ofstream corrupt(path, std::ios::trunc);
        corrupt << "{ this is not json";
    }
    {
        const IdentityStore store(path);
        IdentitySnapshot snapshot;
        std::string error;
        Check(!store.Load(snapshot, error),
            "A corrupt identity file loaded as though it were a fresh personality.");
        Check(!error.empty(), "A refused load gave no reason.");
    }

    // And a file from a newer build is refused rather than partially misread.
    {
        std::ofstream newer(path, std::ios::trunc);
        newer << R"({"schemaVersion": )" << (IdentitySchemaVersion + 1) << "}";
    }
    {
        const IdentityStore store(path);
        IdentitySnapshot snapshot;
        std::string error;
        Check(!store.Load(snapshot, error),
            "An identity file from a newer schema was read anyway.");
    }
}

void TestTraitNamesRoundTrip()
{
    for (std::size_t index = 0; index < TraitCount; ++index)
    {
        const auto trait = static_cast<Trait>(index);
        const std::string name = ToString(trait);
        Check(!name.empty() && name != "unknown",
            "A trait has no persisted name at index " + std::to_string(index));
        Check(TraitFromString(name) == trait,
            "Trait name '" + name + "' did not round-trip.");
    }
    Check(TraitFromString("not_a_trait") == Trait::Count,
        "An unknown trait name was mapped onto a real trait.");
}
}

void RunIdentityTests()
{
    TestBaseAndDeltaStaySeparate();
    TestSheStartsChildlikeButNotFixed();
    TestDriftIsExplainedInPlainLanguage();
    TestRelationshipsAreIndependentAndNotAssumedWarm();
    TestFrictionIsNotAVerdict();
    TestRelationshipMovementIsBoundedPerExchange();
    TestIdentitySurvivesRestartAndRefusesToGuess();
    TestTraitNamesRoundTrip();
    std::cout << "Personality keeps its origin, relationships stay independent and "
                 "bounded, and identity survives a restart.\n";
}
