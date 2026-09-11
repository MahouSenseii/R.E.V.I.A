#include "Presentation/avatarState.h"

#include <algorithm>

namespace revia::presentation
{

namespace
{

float Clamp(const float value, const float low, const float high)
{
    return std::max(low, std::min(high, value));
}

// How lively a feeling looks. Not every strong emotion is high energy: being miserable
// intensely is still low energy, and an avatar that bounces when she is sad is worse
// than one that never moves at all.
float EnergyFor(const runtime::AffectState state, const float intensity)
{
    switch (state)
    {
        case runtime::AffectState::Excited:
        case runtime::AffectState::Playful:
            return Clamp(0.65F + intensity * 0.35F, 0.0F, 1.0F);
        case runtime::AffectState::Curious:
        case runtime::AffectState::Pleased:
        case runtime::AffectState::Focused:
            return Clamp(0.5F + intensity * 0.25F, 0.0F, 1.0F);
        case runtime::AffectState::Angry:
        case runtime::AffectState::Frustrated:
            return Clamp(0.55F + intensity * 0.3F, 0.0F, 1.0F);
        case runtime::AffectState::Sad:
        case runtime::AffectState::Melancholy:
        case runtime::AffectState::Lonely:
        case runtime::AffectState::Bored:
        case runtime::AffectState::Sulky:
            return Clamp(0.35F - intensity * 0.25F, 0.0F, 1.0F);
        case runtime::AffectState::Concerned:
        case runtime::AffectState::Confused:
            return Clamp(0.45F - intensity * 0.1F, 0.0F, 1.0F);
        case runtime::AffectState::Neutral:
            break;
    }
    return 0.5F;
}

float ValenceFor(const runtime::AffectState state, const float intensity)
{
    switch (state)
    {
        case runtime::AffectState::Pleased:
        case runtime::AffectState::Excited:
        case runtime::AffectState::Playful:
            return Clamp(intensity, 0.0F, 1.0F);
        case runtime::AffectState::Curious:
        case runtime::AffectState::Focused:
            return Clamp(intensity * 0.4F, 0.0F, 1.0F);
        case runtime::AffectState::Sad:
        case runtime::AffectState::Melancholy:
        case runtime::AffectState::Lonely:
        case runtime::AffectState::Angry:
        case runtime::AffectState::Frustrated:
        case runtime::AffectState::Sulky:
            return Clamp(-intensity, -1.0F, 0.0F);
        case runtime::AffectState::Bored:
        case runtime::AffectState::Concerned:
        case runtime::AffectState::Confused:
            return Clamp(-intensity * 0.5F, -1.0F, 0.0F);
        case runtime::AffectState::Neutral:
            break;
    }
    return 0.0F;
}

} // namespace

std::string ToString(const GazeTarget value)
{
    switch (value)
    {
        case GazeTarget::User: return "User";
        case GazeTarget::Screen: return "Screen";
        case GazeTarget::Away: return "Away";
        case GazeTarget::Wandering: return "Wandering";
    }
    return "Unknown";
}

std::string ToString(const IdleMotion value)
{
    switch (value)
    {
        case IdleMotion::Calm: return "Calm";
        case IdleMotion::Attentive: return "Attentive";
        case IdleMotion::Restless: return "Restless";
        case IdleMotion::Subdued: return "Subdued";
    }
    return "Unknown";
}

void PresentationController::OnPresentation(const PresentationEvent& event)
{
    const std::lock_guard<std::mutex> lock(mutex);
    ApplyLocked(event);
}

void PresentationController::ApplyLocked(const PresentationEvent& event)
{
    switch (event.kind)
    {
        case PresentationEventKind::EmotionChanged:
            state.expression = event.affect;
            state.expressionIntensity = Clamp(event.affectIntensity, 0.0F, 1.0F);
            state.energy = EnergyFor(event.affect, state.expressionIntensity);
            state.moodValence = ValenceFor(event.affect, state.expressionIntensity);
            // Low mood softens the idle rather than freezing it. A still avatar reads as
            // broken; a subdued one reads as sad.
            state.idle = state.moodValence < -0.3F ? IdleMotion::Subdued
                : state.expression == runtime::AffectState::Curious ? IdleMotion::Attentive
                : state.energy > 0.75F ? IdleMotion::Restless : IdleMotion::Calm;
            break;

        case PresentationEventKind::MoodChanged:
            state.moodValence = Clamp(event.moodValence, -1.0F, 1.0F);
            state.energy = Clamp(event.energy, 0.0F, 1.0F);
            if (state.moodValence < -0.3F) state.idle = IdleMotion::Subdued;
            break;

        case PresentationEventKind::CuriosityRaised:
            state.gaze = GazeTarget::User;
            state.idle = IdleMotion::Attentive;
            state.expression = runtime::AffectState::Curious;
            state.expressionIntensity =
                std::max(state.expressionIntensity, Clamp(event.affectIntensity, 0.0F, 1.0F));
            state.activity = event.subject;
            break;

        case PresentationEventKind::StartedListening:
            state.listening = true;
            state.gaze = GazeTarget::User;
            state.idle = IdleMotion::Attentive;
            break;
        case PresentationEventKind::StoppedListening:
            state.listening = false;
            break;

        case PresentationEventKind::StartedThinking:
            // A visible state only. Nothing about the content of the thought reaches
            // here, because nothing about it was put in the event.
            state.thinking = true;
            state.gaze = GazeTarget::Away;
            state.activity = "thinking";
            break;
        case PresentationEventKind::StoppedThinking:
            state.thinking = false;
            state.gaze = GazeTarget::User;
            break;

        case PresentationEventKind::StartedSpeaking:
            state.speaking = true;
            state.lipSync = true;
            state.thinking = false;
            state.gaze = GazeTarget::User;
            state.activity = "speaking";
            break;
        case PresentationEventKind::StoppedSpeaking:
        case PresentationEventKind::SpeechInterrupted:
            state.speaking = false;
            // During a song the mouth belongs to the vocal line, so a spoken line ending
            // does not close it. Outside a song, speech is the only thing driving it.
            if (!state.singing) state.lipSync = false;
            state.activity.clear();
            break;

        case PresentationEventKind::UserInterrupted:
            state.speaking = false;
            state.lipSync = false;
            state.listening = true;
            state.gaze = GazeTarget::User;
            state.idle = IdleMotion::Attentive;
            break;

        case PresentationEventKind::StartedSinging:
            state.singing = true;
            state.performanceMode = true;
            // Deliberately not lip syncing yet. A song starts with an intro, and the
            // mouth belongs to the vocal line rather than to the track.
            state.lipSync = false;
            state.gaze = GazeTarget::User;
            state.activity = event.subject;
            break;
        case PresentationEventKind::StoppedSinging:
            state.singing = false;
            state.performanceMode = false;
            state.lipSync = false;
            state.activity.clear();
            break;
        case PresentationEventKind::VocalStarted:
            state.lipSync = true;
            break;
        case PresentationEventKind::VocalEnded:
            state.lipSync = false;
            break;

        case PresentationEventKind::StartedResearching:
            state.researching = true;
            state.gaze = GazeTarget::Screen;
            state.activity = event.subject.empty() ? "researching" : event.subject;
            break;
        case PresentationEventKind::StoppedResearching:
            state.researching = false;
            state.activity.clear();
            break;

        case PresentationEventKind::StartedComputerTask:
            state.computerTask = true;
            state.gaze = GazeTarget::Screen;
            state.idle = IdleMotion::Attentive;
            state.activity = event.subject.empty() ? "working" : event.subject;
            break;
        case PresentationEventKind::StoppedComputerTask:
            state.computerTask = false;
            state.activity.clear();
            break;

        case PresentationEventKind::GoalCompleted:
        case PresentationEventKind::GoalFailed:
            state.computerTask = false;
            state.researching = false;
            state.activity.clear();
            break;

        case PresentationEventKind::BecameIdle:
            state.thinking = false;
            state.speaking = false;
            state.researching = false;
            state.computerTask = false;
            state.lipSync = state.singing;
            state.gaze = state.singing ? GazeTarget::User : GazeTarget::Wandering;
            state.activity.clear();
            break;
    }
}

AvatarState PresentationController::Snapshot() const
{
    const std::lock_guard<std::mutex> lock(mutex);
    return state;
}

} // namespace revia::presentation
