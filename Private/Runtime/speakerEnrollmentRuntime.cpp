#include "Runtime/reviaSession.h"
#include "Speech/speakerEnrollmentDialogue.h"

namespace revia::runtime
{

void ReviaSession::InitializeSpeakerEnrollment()
{
    speech::SpeakerEnrollmentCallbacks callbacks;
    callbacks.admits = [this](const auto& context) { return SpeakerEnrollmentAdmitted(context); };
    callbacks.provider = std::make_shared<speech::WaveSpeakerRecognitionProvider>();
    callbacks.commit = [this](const auto& context, const auto& features, const auto& admission, std::string& outError)
    {
        std::lock_guard lock(speakerEnrollmentMutationMutex);
        const auto current = [this, context, admission] { return SpeakerEnrollmentAdmitted(context) && admission && admission(); };
        return relationships.SaveConsentedSpeakerSample(context.entityId, features, current, outError);
    };
    speakerEnrollment = std::make_unique<speech::SpeakerEnrollmentDialogue>(std::move(callbacks));
}

void ReviaSession::CancelSpeakerEnrollment()
{
    std::lock_guard lock(speakerEnrollmentMutationMutex);
    ++speakerEnrollmentEpoch;
    if (speakerEnrollment)
        speakerEnrollment->Cancel();
}

void ReviaSession::ConfigureSpeakerResolver()
{
    const auto origin = sessionIdentity.Stamp();
    speechRecognitionService.SetSpeakerResolver(
        [this, origin](const std::filesystem::path& wave, const std::stop_token token)
        {
            if (!sessionIdentity.IsCurrent(origin) || token.stop_requested())
                return identity::SpeakerObservation{};
            std::string message;
            (void)CaptureSpeakerEnrollmentSample(wave, token, message);
            if (!message.empty() && sessionIdentity.IsCurrent(origin) && !token.stop_requested())
                PublishComponent("Microphone", "EnrollmentSample", message);
            return speech::SpeakerRecognition(relationships).Observe(wave, relationships.RecognitionConsentRevisions(), token);
        });
}

bool ReviaSession::SpeakerEnrollmentAdmitted(const speech::SpeakerEnrollmentContext& context) const
{
    if (!started.load() || !sessionIdentity.IsCurrent(context.stamp) || !Admits(context.stamp) ||
        context.stamp.policyVersion != companionAuthority->Revision() || context.enrollmentEpoch != speakerEnrollmentEpoch.load())
        return false;
    std::lock_guard lock(audienceMutex);
    return configuredAudience.kind == identity::AudienceKind::Private;
}

bool ReviaSession::TryHandleSpeakerEnrollment(const agents::InputBatch& input, SessionResult& result)
{
    if (!speakerEnrollment)
        return false;
    speech::SpeakerEnrollmentResult dialogue;
    std::string selectedPerson;
    if (input.source == agents::InputSource::Typed && input.context.audience.kind == identity::AudienceKind::Private)
    {
        speech::SpeakerEnrollmentContext context;
        {
            std::lock_guard lock(speakerEnrollmentMutationMutex);
            if (!InputContextCurrent(input.context))
                return false;
            context = {input.context.stamp, speakerEnrollmentEpoch.load(), input.context.participantId};
        }
        if (const auto person = relationships.Find(context.entityId))
        {
            selectedPerson = person->displayName.empty() ? "the current local participant" : person->displayName;
            dialogue = speakerEnrollment->HandleTyped(input.text, context);
        }
        else
            dialogue = speakerEnrollment->HandleVoiceRequest(input.text);
    }
    else
    {
        dialogue = speakerEnrollment->HandleVoiceRequest(input.text);
    }
    if (!dialogue.handled)
        return false;
    result.succeeded = true;
    result.fromAssistant = true;
    result.text = dialogue.message;
    if (!selectedPerson.empty() && dialogue.phase == speech::SpeakerEnrollmentPhase::AwaitingConsent)
        result.text = "For " + selectedPerson + ": " + result.text;
    result.reasoning = dialogue.completed ? "The explicitly confirmed sample has a durable private identity receipt."
                                          : "Recognition requires separate consent and sample confirmation.";
    return true;
}

bool ReviaSession::CaptureSpeakerEnrollmentSample(
    const std::filesystem::path& wave, const std::stop_token stopToken, std::string& outMessage)
{
    outMessage.clear();
    if (!speakerEnrollment)
        return false;
    const auto observed = speakerEnrollment->CaptureNextSample(wave, stopToken);
    outMessage = observed.message;
    return observed.handled && observed.phase == speech::SpeakerEnrollmentPhase::AwaitingConfirmation;
}

}
