#include "Identity/relationshipRegistry.h"
#include "Speech/addresseeGate.h"
#include "Speech/speakerRecognition.h"
#include "Initiative/initiativeController.h"
#include "testSupport.h"

#include <chrono>
#include <iostream>
#include <string>
#include <fstream>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

namespace
{
using namespace std::chrono_literals;
using revia::tests::Check;

void TestFreshSelectionDoesNotGuessSoleHistoricalPerson()
{
    revia::tests::ScopedTestDirectory directory;
    revia::identity::RelationshipRegistry identities(directory.root / "identity.json");
    const std::string person = identities.ResolveNamedLocalSpeaker("Fixture Sam");
    Check(!person.empty() && person != revia::identity::LocalUserEntityId(), "The named-person fixture was not established.");
    Check(identities.DefaultLocalSpeaker() == revia::identity::LocalUserEntityId(),
        "A fresh selection assumes the sole historical named person is currently present.");
}

void TestEvidenceReceiptIsAppliedOnce()
{
    revia::tests::ScopedTestDirectory directory;
    revia::identity::RelationshipRegistry identities(directory.root / "identity.json");
    revia::identity::RelationshipEvent event;
    event.entityId = "local:fixture-sam";
    event.evidenceId = "fixture-social-receipt";
    event.positiveInteraction = 0.8F;
    const auto first = identities.Apply(event);
    const auto duplicate = identities.Apply(event);
    Check(first.interactionCount == 1 && duplicate.interactionCount == first.interactionCount,
        "The same captured social evidence receipt is applied more than once.");
}

void TestReconfigurationClearsOldFollowUpWindow()
{
    revia::speech::AddresseeGate gate;
    const auto now = revia::speech::AddresseeGate::Clock::now();
    Check(gate.Accept("Revia, explain the build.", now, false), "The initial addressed exchange was not admitted.");
    revia::speech::AddresseeSettings replacement;
    replacement.wakeWords = {"fixture-companion-b"};
    gate.Configure(replacement);
    Check(!gate.Accept("and the next step", now + 1s, false), "A reconfigured companion inherits the previous audience follow-up window.");
}

void TestScopedFollowUpAndForeground()
{
    revia::speech::AddresseeGate gate;
    const auto now = revia::speech::AddresseeGate::Clock::now();
    Check(gate.Accept("Revia explain the build", now, false, "sam", "room", 1, true), "Addressed scoped turn denied.");
    Check(gate.Accept("and the next step", now + 1s, false, "sam", "room", 1, true), "Same participant follow-up denied.");
    Check(!gate.Accept("and the next step", now + 2s, false, "pat", "room", 1, true), "Another participant borrowed follow-up.");
    Check(!gate.Accept("and the next step", now + 2s, false, "sam", "room", 2, true), "Changed audience revision borrowed follow-up.");
    Check(!gate.Accept("Revia explain", now + 2s, false, "sam", "room", 1, false), "Background turn was admitted.");
    Check(!gate.Accept("and the next step", now + 2s, false, "", "room", 1, true), "Unknown participant borrowed follow-up.");
    gate.Reset();
    Check(!gate.Accept("and the next step", now + 3s, false, "sam", "room", 1, true), "Reset retained follow-up.");
}

void TestCorrectionReplaysOnlyRetainedSocialEvidence()
{
    revia::tests::ScopedTestDirectory directory;
    revia::identity::RelationshipRegistry registry(directory.root / "identity.json");
    revia::identity::RelationshipEvent wrong;
    wrong.entityId = "sam";
    wrong.evidenceId = "wrong";
    wrong.negativeInteraction = 1;
    registry.Apply(wrong);
    auto good = wrong;
    good.evidenceId = "later";
    good.negativeInteraction = 0;
    good.positiveInteraction = 1;
    registry.Apply(good);
    const auto clock = std::chrono::steady_clock::now();
    registry.SettleAll(clock + 1h, 1ms);
    std::string error;
    Check(registry.CorrectEvidence("wrong", "pat", error), "Retained attribution correction failed.");
    Check(registry.Get("sam").interactionCount == 1 && registry.Get("pat").interactionCount == 1,
        "Correction erased later valid history or counted twice.");
    const auto before = registry.Get("sam");
    Check(registry.CorrectEvidence("wrong", "pat", error), "Idempotent correction failed.");
    Check(registry.Get("sam").interactionCount == before.interactionCount, "Repeated correction changed history.");
    Check(registry.RetainedEvidence().front().originalEntityId == "sam", "Correction lost original attribution.");
    Check(registry.Save(error), "Corrected and settled evidence did not save.");
    revia::identity::RelationshipRegistry restored(directory.root / "identity.json");
    Check(restored.Load(error) && restored.Get("sam").interactionCount == 1, "Persisted settling evidence could not reload completely.");
    for (int index = 0; index < 300; ++index)
    {
        good.evidenceId = "bounded-" + std::to_string(index);
        registry.Apply(good);
    }
    Check(registry.RetainedEvidence().size() == 256 && !registry.CorrectEvidence("wrong", "sam", error),
        "Compacted receipt was guessed/replayed.");
    Check(registry.Get("sam").interactionCount == 301, "Compaction lost valid later evidence.");
}

void TestRecipientAliasesAndConsentedTemplatesPersist()
{
    revia::tests::ScopedTestDirectory directory;
    const auto path = directory.root / "identity.json";
    revia::identity::RelationshipRegistry registry(path);
    registry.Get("sam");
    registry.Get("pat");
    std::string error;
    Check(registry.SetAudienceAlias("sam", "public-room", "pat", "Blue", error), "Scoped alias could not be set.");
    revia::identity::AudienceContext audience{revia::identity::AudienceKind::Public, "public-room", 1, {"pat"}};
    Check(registry.DisplayNameForAudience("sam", audience) == "Blue", "Approved recipient alias missing.");
    audience.recipientEntityIds.push_back("other");
    Check(!registry.DisplayNameForAudience("sam", audience), "Alias disclosed to an unapproved recipient.");
    audience.kind = revia::identity::AudienceKind::Unknown;
    Check(!registry.DisplayNameForAudience("sam", audience), "Unknown audience received an alias.");
    revia::speech::SpeakerFeatures sample;
    sample.values[0] = 1;
    Check(!registry.SetSpeakerTemplate("sam", 1, sample, error), "Template enrolled without consent.");
    Check(registry.GrantRecognitionConsent("sam", error), "Consent grant failed.");
    const auto revision = *registry.RecognitionConsentRevision("sam");
    Check(registry.SetSpeakerTemplate("sam", revision, sample, error) && registry.Save(error), "Consented template failed to persist.");
    revia::identity::RelationshipRegistry restored(path);
    Check(restored.Load(error) && restored.SpeakerTemplates().size() == 1, "Schema3 consent/template did not load.");
    Check(restored.RevokeRecognitionConsent("sam", error) && restored.SpeakerTemplates().empty(), "Revoke retained voice template.");
    Check(!restored.SetSpeakerTemplate("sam", revision, sample, error), "Old captured consent revision enrolled after revoke.");
    Check(restored.Get("sam").entityId == "sam", "Voice forget erased relationship identity.");
    Check(restored.Save(error), "Revoked state save failed.");
    Check(registry.Load(error) && registry.SpeakerTemplates().empty() && !registry.RecognitionConsentRevision("sam"),
        "Revoked consent resurrected after reload.");
}

void TestStrictIdentityAndLegacyConsentAbsence()
{
    revia::tests::ScopedTestDirectory directory;
    const auto path = directory.root / "identity.json";
    const std::string legacy = R"({"schemaVersion":2,"relationships":[{"entityId":"sam"}]})";
    std::string error;
    for (const auto& suffix : {std::string("garbage"), std::string("{}"), std::string(1, '\0')})
    {
        {
            std::ofstream file(path, std::ios::binary);
            file << legacy << suffix;
        }
        revia::identity::IdentitySnapshot snapshot;
        Check(!revia::identity::IdentityStore(path).Load(snapshot, error) && snapshot.relationships.empty(),
            "Identity accepted incomplete document or partial state.");
    }
    {
        std::ofstream file(path);
        file << legacy;
    }
    revia::identity::RelationshipRegistry registry(path);
    Check(registry.Load(error) && registry.RecognitionConsentRevisions().empty(), "Legacy identity invented recognition consent.");
    revia::identity::RelationshipEvent bad;
    bad.entityId = "sam";
    bad.positiveInteraction = std::numeric_limits<float>::quiet_NaN();
    Check(registry.Apply(bad).entityId.empty() && registry.RetainedEvidence().empty(), "Invalid numeric social evidence was persisted.");
}

void TestQuietRequestsAreAuthoredBoundedAndExpire()
{
    revia::initiative::InitiativeController controller;
    const auto now = revia::initiative::InitiativeController::QuietClock::now();
    Check(!controller.RequestQuiet("Sam said \"be quiet\"", now), "Reported quiet request changed output policy.");
    Check(!controller.RequestQuiet("I do not want you to be quiet", now), "Negated quiet request changed policy.");
    Check(!controller.RequestQuiet("be quiet for 99 hours", now), "Unbounded quiet duration accepted.");
    Check(
        controller.RequestQuiet("Revia, please be quiet for 2 seconds", now), "Natural quiet request failed without a pending initiative.");
    Check(controller.IsQuiet(now + 1s) && !controller.IsQuiet(now + 2s), "Quiet duration did not expire at its bound.");
    Check(controller.RequestQuiet("quiet please", now) && controller.IsQuiet(now + 4min), "Default quiet interval missing.");
    Check(controller.SpeakingVerdict({}) == revia::initiative::AttentionVerdict::DismissalCooldown,
        "Initiative speaking bypassed quiet policy.");
    Check(controller.RequestQuiet("you can talk now", now) && !controller.IsQuiet(now), "Explicit resume did not clear quiet policy.");
    Check(controller.RequestQuiet("mute for a bit", now), "The approved default quiet phrase was rejected as a duration.");
}

void TestSpeakerEvidenceNeedsCurrentConsentAndSeparation()
{
    revia::tests::ScopedTestDirectory directory;
    revia::identity::RelationshipRegistry registry(directory.root / "identity.json");
    registry.Get("sam");
    registry.Get("pat");
    revia::speech::SpeakerRecognition recognition(registry);
    revia::speech::SpeakerFeatures features;
    features.values[0] = 1;
    std::string error;
    Check(!recognition.Enroll("sam", 1, features, error), "Voice features enabled identity without consent.");
    registry.GrantRecognitionConsent("sam", error);
    const auto revision = *registry.RecognitionConsentRevision("sam");
    Check(recognition.Enroll("sam", revision, features, error), "Consented feature enrollment failed.");
    const auto known = recognition.Resolve(features, registry.RecognitionConsentRevisions());
    Check(known.entityId == "sam" && known.source == revia::identity::SpeakerSource::ConsentedVoice && !known.observationId.empty(),
        "Consented match did not carry evidence origin.");
    Check(recognition.Resolve(features, {}).entityId.empty(), "Unapproved captured scope enabled voice recognition.");
    registry.GrantRecognitionConsent("pat", error);
    recognition.Enroll("pat", *registry.RecognitionConsentRevision("pat"), features, error);
    Check(recognition.Resolve(features, registry.RecognitionConsentRevisions()).entityId.empty(),
        "Ambiguous voice features guessed a label.");
    recognition.Forget("sam");
    Check(!registry.RecognitionConsentRevision("sam") && registry.Find("sam"), "Forget failed to revoke or erased social history.");
}
} // namespace

void RunSocialIdentityTests()
{
    unsigned failures = 0;
    const auto run = [&failures](const char* name, const auto& test)
    {
        try
        {
            test();
            std::cout << name << ": PASS\n";
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << name << ": " << error.what() << '\n';
        }
    };
    run("TestFreshSelectionDoesNotGuessSoleHistoricalPerson", TestFreshSelectionDoesNotGuessSoleHistoricalPerson);
    run("TestEvidenceReceiptIsAppliedOnce", TestEvidenceReceiptIsAppliedOnce);
    run("TestReconfigurationClearsOldFollowUpWindow", TestReconfigurationClearsOldFollowUpWindow);
    run("TestScopedFollowUpAndForeground", TestScopedFollowUpAndForeground);
    run("TestCorrectionReplaysOnlyRetainedSocialEvidence", TestCorrectionReplaysOnlyRetainedSocialEvidence);
    run("TestRecipientAliasesAndConsentedTemplatesPersist", TestRecipientAliasesAndConsentedTemplatesPersist);
    run("TestStrictIdentityAndLegacyConsentAbsence", TestStrictIdentityAndLegacyConsentAbsence);
    run("TestQuietRequestsAreAuthoredBoundedAndExpire", TestQuietRequestsAreAuthoredBoundedAndExpire);
    run("TestSpeakerEvidenceNeedsCurrentConsentAndSeparation", TestSpeakerEvidenceNeedsCurrentConsentAndSeparation);
    Check(failures == 0, "Social identity acceptance assertions failed: " + std::to_string(failures));
    std::cout << "Social identity tests passed.\n";
}
