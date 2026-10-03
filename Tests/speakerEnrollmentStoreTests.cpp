#include "Identity/identityStore.h"
#include "Identity/relationshipRegistry.h"
#include "Speech/speakerRecognition.h"
#include "testSupport.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>

namespace
{
using namespace revia;
using tests::Check;

speech::SpeakerFeatures Sample(const std::size_t component = 0)
{
    speech::SpeakerFeatures features;
    features.values.at(component) = 1.0F;
    return features;
}

std::string Bytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool SaveSample(identity::RelationshipRegistry& registry, const std::string& entityId, const speech::SpeakerFeatures& features,
    const std::function<bool()>& admission, std::string& error)
{
    return registry.SaveConsentedSpeakerSample(entityId, features, admission, error);
}

void CheckTemplate(const identity::RelationshipRegistry& registry, const std::string& entityId, const std::uint64_t revision,
    const speech::SpeakerFeatures& features)
{
    const auto templates = registry.SpeakerTemplates();
    const auto found = std::find_if(templates.begin(), templates.end(), [&](const auto& sample) { return sample.entityId == entityId; });
    Check(found != templates.end() && found->consentRevision == revision && found->features.version == features.version &&
              found->features.values == features.values,
        "Enrollment changed or lost the expected consented sample.");
}

void TestFailedSavePreservesOldSampleAndRetry()
{
    tests::ScopedTestDirectory directory;
    const auto path = directory.root / "identity.json";
    identity::RelationshipRegistry registry(path);
    std::string error;
    registry.Get("sam");
    registry.Get("new-person");
    Check(registry.GrantRecognitionConsent("sam", error), "Old explicit consent fixture failed.");
    const auto revision = *registry.RecognitionConsentRevision("sam");
    Check(registry.SetSpeakerTemplate("sam", revision, Sample(), error) && registry.Save(error), "Old durable sample fixture failed.");
    const std::string original = Bytes(path);
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    std::filesystem::create_directory(temporary);
    Check(!SaveSample(
              registry, "sam", Sample(1), [] { return true; }, error) &&
              !error.empty(),
        "A blocked temporary identity file reported enrollment success.");
    CheckTemplate(registry, "sam", revision, Sample());
    Check(Bytes(path) == original, "Failed replacement changed the durable prior identity.");
    Check(!SaveSample(registry, "new-person", Sample(2), [] { return true; }, error), "Blocked initial enrollment reported success.");
    Check(!registry.RecognitionConsentRevision("new-person"), "Failed initial enrollment granted live consent.");
    Check(registry.SpeakerTemplates().size() == 1 && Bytes(path) == original, "Failed initial enrollment retained candidate material.");
    std::filesystem::remove(temporary);
    Check(
        SaveSample(registry, "sam", Sample(1), [] { return true; }, error), "Enrollment could not retry after a controlled save failure.");
    CheckTemplate(registry, "sam", revision, Sample(1));
    identity::RelationshipRegistry reloaded(path);
    Check(reloaded.Load(error), "Retried identity did not load.");
    CheckTemplate(reloaded, "sam", revision, Sample(1));
}

void TestDurableGrantPreservesOtherIdentityAndBoundedReplacement()
{
    tests::ScopedTestDirectory directory;
    const auto path = directory.root / "identity.json";
    identity::RelationshipRegistry registry(path);
    std::string error;
    registry.Get("sam");
    registry.SetDisplayName("sam", "Synthetic Sam");
    registry.Get("other");
    registry.SetDisplayName("other", "Synthetic Other");
    registry.ReinforcePreference("synthetic astronomy", true, identity::PreferenceSource::Observed);
    auto development = registry.Development();
    development.delta[identity::Trait::Patience] = 0.08F;
    registry.SetDevelopment(development);
    auto mood = registry.Mood();
    mood.energy = 0.31F;
    registry.SetMood(mood);
    Check(registry.SetAudienceAlias("other", "fixture-audience", "sam", "Known colleague", error), "Alias fixture failed.");
    identity::RelationshipEvent event;
    event.entityId = "other";
    event.evidenceId = "synthetic-support-event";
    event.positiveInteraction = 0.5F;
    event.cooperation = 0.4F;
    event.description = "synthetic support";
    registry.Apply(event);
    Check(registry.GrantRecognitionConsent("other", error), "Other consent fixture failed.");
    const auto otherRevision = *registry.RecognitionConsentRevision("other");
    Check(registry.SetSpeakerTemplate("other", otherRevision, Sample(3), error) && registry.Save(error), "Other sample fixture failed.");
    auto original = nlohmann::json::parse(Bytes(path));
    Check(SaveSample(registry, "sam", Sample(), [] { return true; }, error), "Initial atomic enrollment failed: " + error);
    const auto revision = registry.RecognitionConsentRevision("sam");
    Check(revision && *revision == 1, "Initial explicit grant did not have a durable revision.");
    for (std::size_t component = 1; component < 8; ++component)
        Check(SaveSample(registry, "sam", Sample(component), [] { return true; }, error), "Confirmed bounded replacement failed.");
    Check(registry.SpeakerTemplates().size() == 2, "Replacement appended unbounded samples.");
    CheckTemplate(registry, "sam", *revision, Sample(7));
    CheckTemplate(registry, "other", otherRevision, Sample(3));
    identity::RelationshipRegistry reloaded(path);
    Check(reloaded.Load(error), "Committed consent and sample did not durably reload.");
    CheckTemplate(reloaded, "sam", *revision, Sample(7));
    auto actual = nlohmann::json::parse(Bytes(path));
    for (auto* document : {&original, &actual})
    {
        auto& consents = (*document)["recognitionConsents"];
        consents.erase(std::remove_if(consents.begin(), consents.end(), [](const auto& item) { return item.at("entityId") == "sam"; }),
            consents.end());
        auto& templates = (*document)["speakerTemplates"];
        templates.erase(std::remove_if(templates.begin(), templates.end(), [](const auto& item) { return item.at("entityId") == "sam"; }),
            templates.end());
    }
    Check(actual == original, "Enrollment changed unrelated identity, social evidence, preferences, aliases or another sample.");
}

void TestDeniedThrowingAndStaleAdmissionPreserveIdentity()
{
    tests::ScopedTestDirectory directory;
    const auto path = directory.root / "identity.json";
    identity::RelationshipRegistry registry(path);
    std::string error;
    registry.Get("sam");
    Check(registry.GrantRecognitionConsent("sam", error), "Admission fixture consent failed.");
    const auto revision = *registry.RecognitionConsentRevision("sam");
    Check(registry.SetSpeakerTemplate("sam", revision, Sample(), error) && registry.Save(error), "Admission fixture sample failed.");
    const auto original = Bytes(path);
    Check(!SaveSample(registry, "sam", Sample(1), {}, error), "Null admission authorized enrollment.");
    Check(!SaveSample(registry, "sam", Sample(1), [] { return false; }, error), "Denied admission authorized enrollment.");
    Check(!SaveSample(
              registry, "sam", Sample(1), []() -> bool { throw std::runtime_error("synthetic private guard payload"); }, error),
        "Throwing admission authorized enrollment or escaped.");
    Check(
        error.find("synthetic private guard payload") == std::string::npos, "Guard exception text escaped the fixed enrollment boundary.");
    int observations = 0;
    Check(!SaveSample(
              registry, "sam", Sample(1), [&] { return ++observations == 1; }, error) &&
              observations >= 2,
        "Admission that became stale before persistence committed enrollment.");
    CheckTemplate(registry, "sam", revision, Sample());
    Check(Bytes(path) == original, "Denied or stale enrollment changed durable identity.");
}

void TestInvalidEntityAndFeaturesCannotGrantConsent()
{
    tests::ScopedTestDirectory directory;
    const auto path = directory.root / "identity.json";
    identity::RelationshipRegistry registry(path);
    std::string error;
    registry.Get("sam");
    Check(registry.Save(error), "Validation fixture identity failed.");
    const auto original = Bytes(path);
    for (const std::string entity : std::vector<std::string>{"", "unknown", std::string(161, 'x'), "invalid\nentity"})
        Check(!SaveSample(registry, entity, Sample(), [] { return true; }, error), "Invalid or unknown entity enrolled.");
    std::vector<speech::SpeakerFeatures> invalid(5);
    invalid[1] = Sample();
    invalid[1].version = speech::SpeakerFeatureVersion + 1;
    invalid[2].values[0] = std::numeric_limits<float>::quiet_NaN();
    invalid[3].values[0] = std::numeric_limits<float>::infinity();
    invalid[4].values[0] = 1.01F;
    for (const auto& features : invalid)
        Check(!SaveSample(registry, "sam", features, [] { return true; }, error), "Invalid or incompatible features enrolled.");
    Check(!registry.RecognitionConsentRevision("sam") && registry.SpeakerTemplates().empty() && Bytes(path) == original,
        "Rejected features granted consent, retained a sample or changed durable identity.");
}

void TestConsentCapacityAndRevisionExhaustionRemainBounded()
{
    tests::ScopedTestDirectory directory;
    const auto path = directory.root / "identity.json";
    identity::RelationshipRegistry registry(path);
    std::string error;
    for (int index = 0; index < 128; ++index)
    {
        const std::string entity = "person-" + std::to_string(index);
        registry.Get(entity);
        Check(registry.GrantRecognitionConsent(entity, error), "Capacity fixture consent failed.");
        Check(
            registry.SetSpeakerTemplate(entity, *registry.RecognitionConsentRevision(entity), Sample(), error), "Capacity sample failed.");
    }
    registry.Get("overflow");
    Check(registry.Save(error), "Capacity fixture save failed.");
    const auto original = Bytes(path);
    Check(!SaveSample(
              registry, "overflow", Sample(1), [] { return true; }, error) &&
              !registry.RecognitionConsentRevision("overflow"),
        "Enrollment exceeded consent/sample capacity.");
    Check(Bytes(path) == original && registry.SpeakerTemplates().size() == 128, "Overflow enrollment changed existing samples.");
    Check(SaveSample(
              registry, "person-0", Sample(1), [] { return true; }, error) &&
              registry.SpeakerTemplates().size() == 128,
        "Capacity incorrectly blocked replacement of an existing sample.");
    const auto exhaustedPath = directory.root / "exhausted.json";
    identity::IdentitySnapshot exhausted;
    identity::RelationshipState person;
    person.entityId = "sam";
    exhausted.relationships.emplace("sam", person);
    exhausted.recognitionConsents.emplace("sam", identity::RecognitionConsent{false, std::numeric_limits<std::uint64_t>::max()});
    identity::IdentityStore exhaustedStore(exhaustedPath);
    Check(exhaustedStore.Save(exhausted, error), "Revision exhaustion fixture failed.");
    identity::RelationshipRegistry exhaustedRegistry(exhaustedPath);
    Check(exhaustedRegistry.Load(error), "Revision exhaustion fixture did not load.");
    const auto exhaustedBytes = Bytes(exhaustedPath);
    Check(!SaveSample(
              exhaustedRegistry, "sam", Sample(), [] { return true; }, error) &&
              !exhaustedRegistry.RecognitionConsentRevision("sam") && Bytes(exhaustedPath) == exhaustedBytes,
        "Exhausted consent revision wrapped or committed.");
}

void TestRevokedConsentRegrantsOnlyAfterDurableSave()
{
    tests::ScopedTestDirectory directory;
    const auto path = directory.root / "identity.json";
    identity::RelationshipRegistry registry(path);
    std::string error;
    registry.Get("sam");
    Check(SaveSample(registry, "sam", Sample(), [] { return true; }, error), "Revocation fixture initial enrollment failed.");
    Check(
        registry.RevokeRecognitionConsent("sam", error) && registry.Save(error), "Revocation fixture could not durably withdraw consent.");
    const auto revoked = Bytes(path);
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    std::filesystem::create_directory(temporary);
    Check(!SaveSample(registry, "sam", Sample(2), [] { return true; }, error), "Failed re-enrollment reported success.");
    Check(!registry.RecognitionConsentRevision("sam") && registry.SpeakerTemplates().empty() && Bytes(path) == revoked,
        "Failed re-enrollment restored revoked consent or its old sample.");
    std::filesystem::remove(temporary);
    Check(SaveSample(registry, "sam", Sample(2), [] { return true; }, error), "Confirmed re-enrollment could not retry.");
    Check(registry.RecognitionConsentRevision("sam") == 3, "Confirmed re-enrollment did not advance the withdrawn revision.");
    identity::RelationshipRegistry reloaded(path);
    Check(reloaded.Load(error), "Confirmed re-enrollment did not reload.");
    CheckTemplate(reloaded, "sam", 3, Sample(2));
}
}

void RunSpeakerEnrollmentStoreTests()
{
    TestFailedSavePreservesOldSampleAndRetry();
    TestDurableGrantPreservesOtherIdentityAndBoundedReplacement();
    TestDeniedThrowingAndStaleAdmissionPreserveIdentity();
    TestInvalidEntityAndFeaturesCannotGrantConsent();
    TestConsentCapacityAndRevisionExhaustionRemainBounded();
    TestRevokedConsentRegrantsOnlyAfterDurableSave();
    std::cout << "Speaker enrollment store tests passed (6 actual private-store transactions; no device or recognition accuracy claim).\n";
}
