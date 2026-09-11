#pragma once

#include "Presentation/presentationBus.h"
#include "Presentation/presentationEvents.h"
#include "Runtime/affectTypes.h"

#include <mutex>
#include <string>

namespace revia::presentation
{

// Where she is looking. Named intents rather than angles, because "attentive" survives
// a change of renderer and "head yaw 12 degrees" does not.
enum class GazeTarget
{
    User,
    Screen,
    Away,
    Wandering
};

// How the body behaves when nothing else is happening.
enum class IdleMotion
{
    Calm,
    Attentive,
    Restless,
    Subdued
};

[[nodiscard]] std::string ToString(GazeTarget value);
[[nodiscard]] std::string ToString(IdleMotion value);

// Avatar-neutral visible state.
//
// This is what a renderer reads. Nothing in it names a bone, a parameter, a texture, or a
// model format, which is the point: a Live2D sink and a VRM sink consume the same struct
// and disagree only about how to draw it.
struct AvatarState
{
    runtime::AffectState expression = runtime::AffectState::Neutral;
    float expressionIntensity = 0.25F;

    GazeTarget gaze = GazeTarget::User;
    IdleMotion idle = IdleMotion::Calm;

    bool speaking = false;
    bool listening = false;
    // A visible "she is working" state. Never accompanied by what she is working on.
    bool thinking = false;
    bool singing = false;
    bool researching = false;
    bool computerTask = false;

    // Mouth activity. True while speech is playing, and during a song only while the
    // vocal line is actually sounding -- a three-minute instrumental with a mouth
    // flapping through it is the tell that a rig is driven by the wrong signal.
    bool lipSync = false;
    // A song is a performance, not a conversation: different idle, different framing.
    bool performanceMode = false;

    // 0 flat .. 1 lively.
    float energy = 0.5F;
    // -1 miserable .. +1 delighted.
    float moodValence = 0.0F;
    bool blinkEnabled = true;

    // A short, safe label for whatever she is currently doing, for a debug overlay.
    std::string activity;
};

// Turns presentation events into visible state, deterministically.
//
// Deterministic on purpose, and runtime-owned. A model may later be allowed to offer
// high-level style hints, but the mapping from "she is curious" to "attentive gaze"
// stays here, where it can be tested and cannot be talked out of by generated text.
//
// It is itself a sink, so it hangs off the same bus a renderer does and sees exactly
// what a renderer would see.
class PresentationController : public IPresentationSink
{
public:
    void OnPresentation(const PresentationEvent& event) override;
    [[nodiscard]] std::string Name() const override { return "AvatarState"; }

    [[nodiscard]] AvatarState Snapshot() const;

private:
    void ApplyLocked(const PresentationEvent& event);

    mutable std::mutex mutex;
    AvatarState state;
};

} // namespace revia::presentation
