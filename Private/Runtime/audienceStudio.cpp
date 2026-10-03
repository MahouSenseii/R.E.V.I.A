#include "Runtime/reviaSession.h"

#include <algorithm>

namespace revia::runtime
{

identity::AudienceContext ReviaSession::Audience() const
{
    std::lock_guard lock(audienceMutex);
    auto result = configuredAudience;
    result.kind = lastInputAudience;
    return result;
}

void ReviaSession::InvalidateAudience()
{
    CancelSpeakerEnrollment();
    {
        std::lock_guard lock(audienceMutex);
        ++configuredAudience.revision;
        audienceHistory.clear();
    }
    inputArbiter.Clear();
    addresseeGate.Reset();
    speechCoordinator.CancelAll("the disclosure audience changed");
    std::stop_source active;
    {
        std::lock_guard lock(cancellationMutex);
        active = activeStopSource;
    }
    active.request_stop();
    speechService.StopSpeaking();
    userInteractionGeneration.fetch_add(1);
    PreemptAutonomousActivity("the disclosure audience changed");
}

std::function<bool()> ReviaSession::CaptureSpeechAdmission(const identity::AudienceContext& audience, const RuntimeStamp& origin) const
{
    return [this, audience, origin]
    {
        const auto current = Audience();
        return started.load() && Admits(origin) && audience.kind == identity::AudienceKind::Private &&
            current.kind == audience.kind && current.revision == audience.revision && !initiativeController.IsQuiet();
    };
}

bool ReviaSession::SetAudience(identity::AudienceContext audience, std::string& outError)
{
    outError.clear();
    if (audience.audienceId.empty() || audience.audienceId.size() > 64 || audience.recipientEntityIds.size() > 16 ||
        std::any_of(audience.recipientEntityIds.begin(), audience.recipientEntityIds.end(),
            [](const auto& id) { return id.empty() || id.size() > 128; }) ||
        audience.kind < identity::AudienceKind::Unknown || audience.kind > identity::AudienceKind::Public)
    {
        outError = "The audience needs a bounded identifier and recipient list.";
        return false;
    }
    CancelSpeakerEnrollment();
    {
        std::lock_guard lock(audienceMutex);
        audience.revision = configuredAudience.revision + 1;
        configuredAudience = std::move(audience);
        audienceExplicit = true;
        lastInputAudience = configuredAudience.kind;
        lastInputParticipant.clear();
        audienceHistory.clear();
    }
    InvalidateAudience();
    return true;
}

agents::InputContext ReviaSession::CaptureInputContext(const agents::InputSource source, const identity::SpeakerObservation& observed)
{
    agents::InputContext result;
    result.stamp = sessionIdentity.Stamp();
    result.stamp.policyVersion = companionAuthority->Revision();
    identity::SpeakerObservation speaker = observed;
    if (speaker.source == identity::SpeakerSource::ConsentedVoice)
    {
        const auto consent = relationships.RecognitionConsentRevision(speaker.entityId);
        if (!consent || *consent != speaker.consentRevision)
        {
            speaker = {};
        }
    }
    if (source == agents::InputSource::Typed)
    {
        std::lock_guard lock(speakerMutex);
        result.participantId = currentSpeakerId;
    }
    else
    {
        result.participantId = speaker.source == identity::SpeakerSource::ConsentedVoice ? speaker.entityId
            : "unknown:" + (speaker.observationId.empty() ? actions::NewActionId() : speaker.observationId);
        result.participantSource = speaker.source;
        result.consentRevision = speaker.consentRevision;
    }
    std::lock_guard lock(audienceMutex);
    result.audience = configuredAudience;
    if (!audienceExplicit && source != agents::InputSource::Typed)
    {
        result.audience.kind = identity::AudienceKind::Unknown;
        result.audience.audienceId = "local-room";
        result.audience.recipientEntityIds.clear();
    }
    if (source != agents::InputSource::Typed && result.audience.kind == identity::AudienceKind::Private)
    {
        result.audience.kind = speaker.source == identity::SpeakerSource::ConsentedVoice && !result.audience.recipientEntityIds.empty()
            ? identity::AudienceKind::Shared : identity::AudienceKind::Unknown;
    }
    if (lastInputAudience != result.audience.kind || (!lastInputParticipant.empty() && lastInputParticipant != result.participantId))
    {
        ++configuredAudience.revision;
        audienceHistory.clear();
    }
    lastInputAudience = result.audience.kind;
    lastInputParticipant = result.participantId;
    result.audience.revision = configuredAudience.revision;
    return result;
}

bool ReviaSession::InputContextCurrent(const agents::InputContext& captured) const
{
    if (!started.load() || !sessionIdentity.IsCurrent(captured.stamp))
    {
        return false;
    }
    if (captured.participantSource == identity::SpeakerSource::ConsentedVoice)
    {
        const auto revision = relationships.RecognitionConsentRevision(captured.participantId);
        if (!revision || *revision != captured.consentRevision) return false;
    }
    std::lock_guard lock(audienceMutex);
    return captured.audience.revision == configuredAudience.revision && captured.audience.kind == lastInputAudience &&
        captured.participantId == lastInputParticipant;
}

bool ReviaSession::EnrollSpeaker(const std::string& entityId, const std::filesystem::path& wave,
    const bool explicitConsent, std::string& outError)
{
    outError.clear();
    if (!explicitConsent || !relationships.Find(entityId))
    {
        outError = "Recognition needs explicit consent from an existing participant.";
        return false;
    }
    CancelSpeakerEnrollment();
    const auto origin = Stamp();
    const auto epoch = speakerEnrollmentEpoch.load();
    const auto admission = [this, origin, epoch]
    {
        if (!sessionIdentity.IsCurrent(origin) || !Admits(origin) || origin.policyVersion != companionAuthority->Revision() ||
            epoch != speakerEnrollmentEpoch.load())
            return false;
        std::lock_guard lock(audienceMutex);
        return configuredAudience.kind == identity::AudienceKind::Private;
    };
    if (!admission())
    {
        outError = "Recognition enrollment needs a current private control context.";
        return false;
    }
    const auto features = speech::WaveSpeakerRecognitionProvider().Extract(wave, {}, outError);
    if (!features)
        return false;
    {
        std::lock_guard lock(speakerEnrollmentMutationMutex);
        if (!relationships.SaveConsentedSpeakerSample(entityId, *features, admission, outError))
            return false;
    }
    InvalidateAudience();
    return true;
}

bool ReviaSession::ForgetSpeaker(const std::string& entityId, std::string& outError)
{
    if (!relationships.RevokeRecognitionConsent(entityId, outError)) return false;
    speech::SpeakerRecognition(relationships).Forget(entityId);
    InvalidateAudience();
    return relationships.Save(outError);
}

bool ReviaSession::CorrectSpeakerEvidence(const std::string& evidenceId, const std::string& entityId, std::string& outError)
{
    if (!relationships.CorrectEvidence(evidenceId, entityId, outError)) return false;
    InvalidateAudience();
    return relationships.Save(outError);
}

std::vector<identity::RelationshipEvidenceRecord> ReviaSession::RelationshipEvidence() const
{
    return relationships.RetainedEvidence();
}

bool ReviaSession::SetAudienceAlias(const std::string& entityId, const std::string& alias, std::string& outError)
{
    const auto audience = Audience();
    if (audience.recipientEntityIds.empty())
    {
        outError = "Choose the audience recipients before sharing an alias.";
        return false;
    }
    for (const auto& recipient : audience.recipientEntityIds)
    {
        if (!relationships.SetAudienceAlias(entityId, audience.audienceId, recipient, alias, outError)) return false;
    }
    InvalidateAudience();
    return relationships.Save(outError);
}

}
