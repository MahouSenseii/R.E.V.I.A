#pragma once

#include <chrono>
#include <string>

namespace revia::emotion
{

// Where something came from. Not what it means -- that is appraisal's job, and keeping
// the two apart is what lets the same event land differently on different days.
enum class StimulusSource
{
    Conversation,
    Relationship,
    Perception,
    Memory,
    Goal,
    Action,
    Research,
    Environment,
    Internal
};

[[nodiscard]] std::string ToString(StimulusSource source);
[[nodiscard]] StimulusSource StimulusSourceFromString(const std::string& name);

// Runtime-confirmed event with typed appraisal inputs; descriptions are diagnostic only.
// Appraisal must not branch on prose or accept model-invented events.
struct Stimulus
{
    StimulusSource source = StimulusSource::Internal;

    // Coarse machine-readable kind, e.g. "goal_failed", "message_received". Stable
    // enough to group on in training data; never parsed for meaning at runtime.
    std::string eventType;
    // Who this involves, when it involves anyone. Empty for events with no other party.
    // This is the key relationships are stored under.
    std::string subjectId;
    // Human-readable, for logs, the debug panel, and the reason line on a feeling.
    std::string description;

    // -1 thoroughly bad .. 0 neutral .. +1 thoroughly good.
    float valence = 0.0F;
    // How much this mattered. Below the appraisal floor nothing is felt at all, which
    // is the ordinary outcome for most of what happens.
    float importance = 0.5F;
    // How much of it was new. Novelty separates interest from mere satisfaction and
    // surprise from mere disappointment.
    float novelty = 0.0F;
    // How sure the runtime is that this happened as described. A perception event is
    // less certain than a goal exit code, and a feeling should be weaker for it.
    float certainty = 1.0F;

    // Outcome magnitudes, kept separate from valence because a partial success is not
    // the same event as a mild success.
    float success = 0.0F;
    float failure = 0.0F;

    // Confirmed absence of conversation, not an inference about the user's feelings
    // or physical presence. Only the session's quiet-period clock supplies this.
    float quietConversation = 0.0F;
    float idleBoredom = 0.0F;

    // Causation changes the feeling more than the facts do. The same failure is
    // frustration when she chose the approach and concern when something broke
    // underneath her.
    bool selfCaused = false;
    bool userCaused = false;
    bool explicitlyPlayful = false;

    std::chrono::system_clock::time_point timestamp =
        std::chrono::system_clock::now();

    // True when there is enough here to be worth appraising at all.
    [[nodiscard]] bool IsMeaningful(float importanceFloor = 0.2F) const;
};

} // namespace revia::emotion
