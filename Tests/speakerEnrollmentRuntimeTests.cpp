#include "Runtime/reviaSession.h"
#include "Identity/relationshipRegistry.h"
#include "reviaSessionTestAccess.h"
#include "testSupport.h"

#include <iostream>
#include <cmath>
#include <fstream>
#include <numbers>

namespace
{
using namespace revia;
using namespace revia::runtime;
using tests::Check;

void WriteSyntheticWave(const std::filesystem::path& path, const double frequency = 300)
{
    constexpr unsigned rate = 16000, frames = 8000, dataBytes = frames * 2;
    std::ofstream output(path, std::ios::binary);
    const auto little = [&](unsigned value, unsigned count)
    {
        for (unsigned index = 0; index < count; ++index)
            output.put(static_cast<char>(value >> (8 * index)));
    };
    output.write("RIFF", 4);
    little(dataBytes + 36, 4);
    output.write("WAVEfmt ", 8);
    little(16, 4);
    little(1, 2);
    little(1, 2);
    little(rate, 4);
    little(rate * 2, 4);
    little(2, 2);
    little(16, 2);
    output.write("data", 4);
    little(dataBytes, 4);
    for (unsigned index = 0; index < frames; ++index)
        little(static_cast<std::uint16_t>(static_cast<std::int16_t>(10000 * std::sin(2 * std::numbers::pi * frequency * index / rate))), 2);
}

void Prepare(ReviaSession& session)
{
    (void)ReviaSessionTestAccess::People(session).Get(identity::LocalUserEntityId());
    ReviaSessionTestAccess::MarkStudioStarted(session, true);
    ReviaSessionTestAccess::BindStudioSpeakerResolver(session);
}

void Arm(ReviaSession& session)
{
    const auto request = session.Submit("Remember my voice.");
    Check(request.succeeded && request.text.find("consent") != std::string::npos, "The enrollment request did not ask for consent.");
    const auto consent = session.Submit("I consent to local speaker recognition only.");
    Check(consent.succeeded && consent.text.find("not been started") != std::string::npos,
        "Recognition consent did not explain the separately controlled recording.");
}

void TestNaturalRequestUsesConsentDialogue()
{
    using namespace revia::runtime;
    revia::tests::ScopedTestDirectory temporary;
    ReviaSession session(CompanionPaths(temporary.root, {"enrollment-runtime", "Enrollment fixture", "assistant", false}));
    (void)ReviaSessionTestAccess::People(session).Get(revia::identity::LocalUserEntityId());
    ReviaSessionTestAccess::MarkStudioStarted(session, true);
    const auto requested = session.Submit("Remember my voice.");
    revia::tests::Check(requested.succeeded && requested.fromAssistant && requested.text.find("consent") != std::string::npos,
        "The real typed turn did not enter the explicit speaker-consent dialogue.");
    revia::tests::Check(
        ReviaSessionTestAccess::People(session).SpeakerTemplates().empty(), "A request alone enrolled a biometric template.");
}

void TestActualTransientResolverWaitsForConfirmationAndPersists()
{
    tests::ScopedTestDirectory temporary;
    const CompanionPaths paths(temporary.root, {"enrollment-flow", "Enrollment fixture", "assistant", false});
    ReviaSession session(paths);
    Prepare(session);
    const auto revision = session.Authority()->Revision();
    Arm(session);
    const auto wave = temporary.root / "synthetic.wav";
    WriteSyntheticWave(wave);
    const auto beforeConfirmation = ReviaSessionTestAccess::ResolveStudioVoice(session, wave);
    Check(beforeConfirmation.entityId.empty() && ReviaSessionTestAccess::People(session).SpeakerTemplates().empty(),
        "The actual transient resolver stored or guessed identity before independent confirmation.");
    const auto confirmed = session.Submit("Confirm this voice sample.");
    Check(confirmed.succeeded && confirmed.text.find("saved for local recognition only") != std::string::npos, confirmed.text);
    identity::RelationshipRegistry restarted(paths.Resolve("RuntimeData/Identity/identity.json"));
    std::string error;
    Check(restarted.Load(error) && restarted.SpeakerTemplates().size() == 1 &&
              restarted.RecognitionConsentRevision(identity::LocalUserEntityId()).has_value(),
        "The conversational flow has no durable consented template.");
    const auto observed = ReviaSessionTestAccess::ResolveStudioVoice(session, wave);
    Check(observed.entityId == identity::LocalUserEntityId() && session.Authority()->Revision() == revision,
        "Recognition changed authority or failed to match its confirmed controlled sample.");
    const auto voice = ReviaSessionTestAccess::StudioInput(session, agents::InputSource::Voice, observed);
    Check(voice.audience.kind == identity::AudienceKind::Unknown,
        "The confirmed recognition sample upgraded voice to private authorization.");
}

void TestAudienceAndStopWithdrawPendingSample()
{
    tests::ScopedTestDirectory temporary;
    ReviaSession session(CompanionPaths(temporary.root, {"enrollment-cancel", "Enrollment fixture", "assistant", false}));
    Prepare(session);
    const auto wave = temporary.root / "synthetic.wav";
    WriteSyntheticWave(wave);
    Arm(session);
    std::string error;
    Check(session.SetAudience({identity::AudienceKind::Public, "public-fixture", 0, {}}, error), error);
    (void)ReviaSessionTestAccess::ResolveStudioVoice(session, wave);
    Check(session.SetAudience({identity::AudienceKind::Private, "private-fixture", 0, {}}, error), error);
    (void)session.Submit("Confirm this voice sample.");
    Check(ReviaSessionTestAccess::People(session).SpeakerTemplates().empty(), "An audience change retained pending biometric features.");
    Arm(session);
    session.RequestStop();
    (void)ReviaSessionTestAccess::ResolveStudioVoice(session, wave);
    (void)session.Submit("Confirm this voice sample.");
    Check(ReviaSessionTestAccess::People(session).SpeakerTemplates().empty(), "Stop retained or persisted the pending sample.");
}

void TestStalePrivateSubmissionCannotAcquireFreshPurpose()
{
    tests::ScopedTestDirectory temporary;
    ReviaSession session(CompanionPaths(temporary.root, {"enrollment-stale", "Enrollment fixture", "assistant", false}));
    Prepare(session);
    const auto captured = ReviaSessionTestAccess::StudioInput(session, agents::InputSource::Typed);
    std::string error;
    Check(session.SetAudience({identity::AudienceKind::Public, "public-fixture", 0, {}}, error), error);
    SessionResult result;
    const agents::InputBatch stale{"Remember my voice.", captured, agents::InputSource::Typed, true};
    Check(!ReviaSessionTestAccess::HandleStudioEnrollment(session, stale, result) &&
              !ReviaSessionTestAccess::StudioEnrollmentPending(session),
        "An old private submission borrowed the current audience's fresh enrollment epoch.");
}

void TestUnknownVoiceCanWithdrawWithoutAuthorizingEnrollment()
{
    tests::ScopedTestDirectory temporary;
    ReviaSession session(CompanionPaths(temporary.root, {"enrollment-withdraw", "Enrollment fixture", "assistant", false}));
    Prepare(session);
    Arm(session);
    const auto captured = ReviaSessionTestAccess::StudioInput(session, agents::InputSource::Voice);
    SessionResult result;
    const agents::InputBatch cancellation{"Cancel voice enrollment.", captured, agents::InputSource::Voice, true};
    Check(ReviaSessionTestAccess::HandleStudioEnrollment(session, cancellation, result) &&
              !ReviaSessionTestAccess::StudioEnrollmentPending(session) &&
              ReviaSessionTestAccess::People(session).SpeakerTemplates().empty(),
        "Unknown voice withdrawal granted identity or left sample capture armed.");
}

void TestSelectedWaveReplacementFailurePreservesOldTemplate()
{
    tests::ScopedTestDirectory temporary;
    const CompanionPaths paths(temporary.root, {"enrollment-wave", "Enrollment fixture", "assistant", false});
    ReviaSession session(paths);
    Prepare(session);
    const auto wave = temporary.root / "synthetic.wav";
    WriteSyntheticWave(wave);
    std::string error;
    Check(session.EnrollSpeaker(identity::LocalUserEntityId(), wave, true, error), error);
    const auto before = ReviaSessionTestAccess::People(session).SpeakerTemplates().front();
    Check(!session.EnrollSpeaker(identity::LocalUserEntityId(), temporary.root / "absent.wav", true, error),
        "An absent selected WAV was accepted.");
    const auto second = temporary.root / "second.wav";
    WriteSyntheticWave(second, 1400);
    const auto blocked = paths.Resolve("RuntimeData/Identity/identity.json.tmp");
    std::filesystem::create_directory(blocked);
    Check(!session.EnrollSpeaker(identity::LocalUserEntityId(), second, true, error),
        "A deliberately blocked atomic identity save succeeded.");
    const auto after = ReviaSessionTestAccess::People(session).SpeakerTemplates();
    Check(after.size() == 1 && after.front().consentRevision == before.consentRevision &&
              after.front().features.values == before.features.values,
        "A failed selected-WAV replacement erased prior consent or the working template.");
}
}

void RunSpeakerEnrollmentRuntimeTests()
{
    TestNaturalRequestUsesConsentDialogue();
    TestActualTransientResolverWaitsForConfirmationAndPersists();
    TestAudienceAndStopWithdrawPendingSample();
    TestStalePrivateSubmissionCannotAcquireFreshPurpose();
    TestUnknownVoiceCanWithdrawWithoutAuthorizingEnrollment();
    TestSelectedWaveReplacementFailurePreservesOldTemplate();
    std::cout << "Connected conversational speaker enrollment checks passed.\n";
}
