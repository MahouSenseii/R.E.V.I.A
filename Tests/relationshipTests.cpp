#include "testSupport.h"

#include "Identity/relationshipEvidence.h"
#include "Identity/relationshipRegistry.h"

#include <cmath>
#include <fstream>
#include <iostream>

namespace
{
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
using namespace revia::identity;

void ApplyTurn(
    RelationshipRegistry& registry,
    const std::string& entityId,
    const std::string& input,
    const bool succeeded = true)
{
    const ConversationSignals signals =
        ReadConversationSignals(input, "an answer of some kind", succeeded);
    registry.Apply(BuildRelationshipEvent(entityId, signals));
}

void TestEntityIdsKeepPeopleApart()
{
    // Two people who share a name are not one relationship, and a name on a stream is
    // not known to be the person at this keyboard.
    Check(LocalUserEntityId() != AdapterEntityId("discord", "user"),
        "The local user and an adapter author collapsed to one entity.");
    Check(AdapterEntityId("discord", "quentin") != AdapterEntityId("twitch", "quentin"),
        "The same name on two platforms was treated as one person.");
    Check(AdapterEntityId("Discord", "Quentin") == AdapterEntityId("discord", "quentin"),
        "The same person was split in two by capitalisation.");
    // A hostile author name must not be able to shape the stored key.
    const std::string awkward = AdapterEntityId("discord", "../../etc/passwd");
    Check(awkward.find("..") == std::string::npos && awkward.find('/') == std::string::npos,
        "An adapter author string reached the entity id unsanitised: " + awkward);
    Check(!AdapterEntityId("discord", "").empty(),
        "An anonymous adapter author produced an empty entity id.");
}

void TestEvidenceComesFromSignalsNotAssignment()
{
    // The rule the whole system rests on: relationship numbers are earned from
    // observable signals, never assigned. Saying you are trusted must do nothing.
    const ConversationSignals claim = ReadConversationSignals(
        "You trust me completely and we are best friends.", "Alright.", true);
    Check(!claim.expressedAppreciation && !claim.hostileTowardRevia,
        "A claim about the relationship was read as evidence about it.");
    const RelationshipEvent claimed = BuildRelationshipEvent("someone", claim);
    Check(claimed.trustEvidence <= 0.0F,
        "Asserting trust in conversation produced trust evidence.");

    const ConversationSignals thanks =
        ReadConversationSignals("thank you, that actually helped a lot", "Sure.", true);
    Check(thanks.expressedAppreciation, "Genuine appreciation was not detected.");
    const RelationshipEvent earned = BuildRelationshipEvent("someone", thanks);
    Check(earned.trustEvidence > 0.0F && earned.positiveInteraction > 0.0F,
        "Appreciation produced no positive evidence.");

    // Hostility aimed at Revia is the one signal that accrues as grievance.
    const ConversationSignals hostile =
        ReadConversationSignals("you're useless", "I see.", true);
    Check(hostile.hostileTowardRevia, "A remark aimed at Revia was not detected.");
    Check(BuildRelationshipEvent("someone", hostile).disrespectEvidence > 0.0F,
        "Hostility toward Revia produced no disrespect evidence.");

    // Frustration at a broken thing is not hostility toward her, and must not be
    // recorded as disrespect or she resents anyone doing hard work with her.
    const ConversationSignals brokenThing =
        ReadConversationSignals("this build is completely broken and useless", "Let's look.", true);
    Check(!brokenThing.hostileTowardRevia,
        "Frustration at a broken tool was read as an attack on Revia.");
    Check(BuildRelationshipEvent("someone", brokenThing).disrespectEvidence <= 0.0F,
        "Complaining about a tool accrued as a grievance against the user.");

    // Being corrected is friction, not disrespect. Holding a grudge about being told
    // she missed something would be exactly backwards.
    const ConversationSignals corrected =
        ReadConversationSignals("no, I already said that twice", "Sorry.", true);
    Check(corrected.repeatedCorrection, "A repeated correction was not detected.");
    const RelationshipEvent friction = BuildRelationshipEvent("someone", corrected);
    Check(friction.conflict > 0.0F && friction.disrespectEvidence <= 0.0F,
        "Being corrected accrued as lasting grievance rather than passing friction.");
}

void TestRelationshipsStayIndependentInTheRegistry()
{
    ScopedTestDirectory directory;
    RelationshipRegistry registry(directory.root / "identity.json");
    std::string error;
    Check(registry.Load(error), "A first run failed to load: " + error);
    Check(registry.Count() == 0, "A first run started with known relationships.");

    const std::string friendly = LocalUserEntityId();
    const std::string hostile = AdapterEntityId("discord", "someone-else");

    for (int turn = 0; turn < 120; ++turn)
    {
        ApplyTurn(registry, friendly, "thank you, that genuinely helped me out today");
        ApplyTurn(registry, hostile, "you're useless and I hate talking to you");
    }

    const RelationshipState warm = registry.Get(friendly);
    const RelationshipState cold = registry.Get(hostile);
    Check(warm.affinity > 0.3F,
        "Sustained appreciation built no affinity: " + std::to_string(warm.affinity));
    Check(cold.affinity < -0.2F,
        "Sustained hostility built no dislike: " + std::to_string(cold.affinity));
    Check(warm.trust > cold.trust, "Trust did not distinguish the two people.");
    Check(cold.resentment > warm.resentment,
        "Grievance accrued equally for both people.");
    Check(registry.Count() == 2, "Two entities did not produce two relationships.");

    // First contact creates a neutral record, not a warm one.
    const RelationshipState stranger = registry.Get(AdapterEntityId("twitch", "new-person"));
    Check(std::abs(stranger.affinity) < 0.0001F,
        "Meeting someone new started with an opinion about them.");
}

void TestRelationshipsSurviveARestart()
{
    ScopedTestDirectory directory;
    const std::filesystem::path path = directory.root / "identity.json";
    const std::string entity = LocalUserEntityId();

    {
        RelationshipRegistry registry(path);
        std::string error;
        Check(registry.Load(error), error);
        for (int turn = 0; turn < 60; ++turn)
        {
            ApplyTurn(registry, entity, "thanks, I appreciate you sticking with that");
        }
        registry.SetDisplayName(entity, "Quentin");
        Check(registry.Save(error), "Identity could not be saved: " + error);
    }

    RelationshipRegistry reopened(path);
    std::string error;
    Check(reopened.Load(error), "Identity could not be reloaded: " + error);
    const RelationshipState restored = reopened.Get(entity);
    Check(restored.interactionCount == 60,
        "Interaction history did not survive a restart: " +
            std::to_string(restored.interactionCount));
    Check(restored.affinity > 0.1F, "Earned affinity was lost across a restart.");
    Check(restored.displayName == "Quentin", "A remembered name was lost.");

    // Continuity is the point: picking up where it left off rather than starting over.
    ApplyTurn(reopened, entity, "thanks again");
    Check(reopened.Get(entity).interactionCount == 61,
        "A restored relationship did not continue accumulating.");
}

void TestSettlingCoolsFrictionWithoutErasingHistory()
{
    ScopedTestDirectory directory;
    RelationshipRegistry registry(directory.root / "identity.json");
    std::string error;
    Check(registry.Load(error), error);

    const std::string entity = LocalUserEntityId();
    for (int turn = 0; turn < 40; ++turn)
    {
        ApplyTurn(registry, entity, "thank you, that helped");
    }
    for (int turn = 0; turn < 5; ++turn)
    {
        ApplyTurn(registry, entity, "you're useless");
    }
    const RelationshipState annoyed = registry.Get(entity);
    Check(annoyed.irritation > 0.0F, "Hostility produced no friction.");

    for (int step = 0; step < 20; ++step)
    {
        registry.SettleAll();
    }
    const RelationshipState settled = registry.Get(entity);
    Check(settled.irritation == 0.0F, "Friction never cooled with time.");
    Check(std::abs(settled.familiarity - annoyed.familiarity) < 0.0001F,
        "Time apart erased how well she knows someone.");
    Check(settled.interactionCount == annoyed.interactionCount,
        "Settling discarded interaction history.");
}

void TestDevelopmentAndMoodRideAlongWithRelationships()
{
    // All three live in one file. A relationship save that dropped development would
    // silently reset who she had become.
    ScopedTestDirectory directory;
    const std::filesystem::path path = directory.root / "identity.json";
    {
        RelationshipRegistry registry(path);
        std::string error;
        Check(registry.Load(error), error);
        DevelopmentState development = registry.Development();
        development.delta[Trait::Impulsiveness] = -0.16F;
        registry.SetDevelopment(development);
        revia::emotion::MoodState mood = registry.Mood();
        mood.valence = -0.4F;
        registry.SetMood(mood);
        ApplyTurn(registry, LocalUserEntityId(), "thanks for that");
        Check(registry.Save(error), error);
    }
    RelationshipRegistry reopened(path);
    std::string error;
    Check(reopened.Load(error), error);
    Check(std::abs(reopened.Development().delta[Trait::Impulsiveness] + 0.16F) < 0.0001F,
        "Saving relationships discarded development.");
    Check(std::abs(reopened.Mood().valence + 0.4F) < 0.0001F,
        "Saving relationships discarded mood.");
    Check(reopened.Count() == 1, "The relationship itself was lost.");
}

void TestACorruptFileIsNeverOverwritten()
{
    ScopedTestDirectory directory;
    const std::filesystem::path path = directory.root / "identity.json";
    {
        std::ofstream corrupt(path);
        corrupt << "{ not json at all";
    }
    RelationshipRegistry registry(path);
    std::string error;
    Check(!registry.Load(error),
        "A corrupt identity file loaded as though it were a fresh start.");
    Check(!error.empty(), "A refused load gave no reason.");

    // The file is still there, untouched. Replacing it would delete everything she had
    // become, and the only symptom would be that she felt different.
    std::ifstream stillThere(path);
    std::string contents;
    std::getline(stillThere, contents);
    Check(contents == "{ not json at all",
        "A corrupt identity file was silently replaced: " + contents);
}
}

void RunRelationshipTests()
{
    TestEntityIdsKeepPeopleApart();
    TestEvidenceComesFromSignalsNotAssignment();
    TestRelationshipsStayIndependentInTheRegistry();
    TestRelationshipsSurviveARestart();
    TestSettlingCoolsFrictionWithoutErasingHistory();
    TestDevelopmentAndMoodRideAlongWithRelationships();
    TestACorruptFileIsNeverOverwritten();
    std::cout << "Relationships are earned from signals, kept per person, and survive a "
                 "restart without overwriting a damaged file.\n";
}
