#pragma once

#include "Runtime/affectTypes.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace revia::presentation
{

// What Revia's mind tells the outside world about itself.
//
// The boundary matters more than the list. Her core knows "I started singing" and "my
// mood changed"; it does not know that a Live2D model has a parameter called
// ParamMouthOpenY. That mapping belongs to a renderer, and keeping it there is what
// makes a future Live2D model, a VRM avatar, an OBS overlay, and a text debug window
// interchangeable rather than three separate rewrites of the same behaviour.
//
// The other half of the boundary is what may never cross it. Revia's private reasoning
// -- the questions she puts to herself before answering something hard -- is not a
// presentation event and has no field to travel in. "Thinking" here is a visible runtime
// state, meaning she is busy and it is worth showing, and nothing about what she is
// thinking.

enum class PresentationEventKind
{
    EmotionChanged,
    MoodChanged,
    StartedListening,
    StoppedListening,
    // She is working on an answer. A state, not a transcript.
    StartedThinking,
    StoppedThinking,
    StartedSpeaking,
    StoppedSpeaking,
    SpeechInterrupted,
    StartedResearching,
    StoppedResearching,
    StartedComputerTask,
    StoppedComputerTask,
    StartedSinging,
    StoppedSinging,
    // Inside a song: the vocal line came in or dropped out. What drives a mouth during a
    // performance, since a song's audio is not the speech pipeline.
    VocalStarted,
    VocalEnded,
    CuriosityRaised,
    BecameIdle,
    UserInterrupted,
    GoalCompleted,
    GoalFailed
};

[[nodiscard]] std::string ToString(PresentationEventKind kind);

struct PresentationEvent
{
    PresentationEventKind kind = PresentationEventKind::BecameIdle;

    // A short, already-safe label: a song title, a goal name, the subject of a
    // curiosity. Never a sentence of reasoning and never the text being spoken.
    //
    // Length-capped when built, because "short label" is the only thing keeping this
    // from becoming the field somebody eventually pours a chain of thought into.
    std::string subject;

    runtime::AffectState affect = runtime::AffectState::Neutral;
    float affectIntensity = 0.0F;

    // -1 miserable .. +1 delighted. Slower-moving than affect: a mood is the day, an
    // emotion is the moment.
    float moodValence = 0.0F;
    // 0 flat .. 1 lively. What an idle animation should feel like.
    float energy = 0.5F;

    std::uint64_t sequence = 0;
    // Monotonic, so a renderer can time transitions without a clock adjustment
    // reordering them.
    std::chrono::steady_clock::time_point occurredAt{};
};

// The cap applied to `subject` when an event is built. Long enough for a song title or a
// goal name, far too short for a paragraph of reasoning.
inline constexpr std::size_t PresentationSubjectLimit = 96;

// Trims a label to something safe to put on screen: single line, length-capped.
[[nodiscard]] std::string SanitizeSubject(const std::string& value);

} // namespace revia::presentation
