#pragma once

#include "Emotion/appraisalContext.h"
#include "Emotion/emotionModel.h"
#include "Emotion/emotionTypes.h"
#include "Emotion/moodState.h"
#include "Emotion/stimulus.h"
#include "Identity/developmentState.h"
#include "Identity/relationshipState.h"
#include "Runtime/affectTypes.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace revia::emotion
{

// What one appraisal produced, kept whole so it can be logged, shown in the debug panel,
// and later exported as a training example.
//
// The delta and the resulting state are both recorded because only their combination
// explains a feeling: "she became more frustrated" and "she is frustrated" answer
// different questions, and a system whose emotions must be causally understandable owes
// an answer to both.
struct AppraisalOutcome
{
    bool changed = false;
    Stimulus stimulus;
    AppraisalContext context;
    EmotionVector delta;
    EmotionVector emotion;
    MoodState mood;
    std::string modelName;
    // Plain sentence naming what happened and what it produced, for logs and the badge.
    std::string explanation;
};

struct EmotionSnapshot
{
    EmotionVector emotion;
    MoodState mood;
    runtime::AffectSnapshot affect;
    // What most recently caused this, in the stimulus's own words, or empty when
    // nothing recent explains it.
    //
    // Read here rather than through a second call so a cause can never be paired with
    // a feeling it did not produce. A prompt that states an emotion without its cause
    // leaves the model to invent one, and an invented cause is indistinguishable from
    // a hallucinated observation about the room.
    std::string cause;
};

// Owns how Revia currently feels, and is the only thing allowed to change it.
//
// Single purpose on purpose: it holds the emotion vector and mood, applies a model to a
// stimulus, and integrates the result. It does not retrieve memories, does not decide
// relationships, does not talk to a language model, and cannot reach a capability. The
// caller assembles the inputs; this owns the state machine.
//
// Thread-safe because stimuli arrive from conversation, goal, perception, and idle
// workers on different threads.
class EmotionRuntime
{
public:
    explicit EmotionRuntime(std::unique_ptr<IEmotionModel> model = MakeDefaultEmotionModel());

    // Appraises one event and folds the result into current emotion and mood. Returns
    // nothing when the stimulus was not meaningful enough to feel, which is the ordinary
    // outcome for most of what happens to her.
    std::optional<AppraisalOutcome> Observe(
        const Stimulus& stimulus,
        const identity::DevelopmentState& development,
        const identity::RelationshipState* relationship = nullptr,
        std::vector<RelevantMemory> memories = {});

    // Time passing with nothing happening. Emotions fade toward calm; mood eases toward
    // its baseline far more slowly. Called by an idle tick, not by a clock this owns.
    void Settle(float emotionDecayRate = 0.08F);

    // At most one quiet-conversation event between actual incoming messages. Admission
    // shares the appraisal lock so a new message cannot race a stale idle observation.
    std::optional<AppraisalOutcome> ObserveQuietConversation(
        const identity::DevelopmentState& development,
        std::chrono::milliseconds quietInterval = std::chrono::minutes(20),
        bool occupied = false, float boredom = 0.0F);

    [[nodiscard]] EmotionVector Emotion() const;
    [[nodiscard]] MoodState Mood() const;
    [[nodiscard]] EmotionSnapshot Current() const;
    // How long a cause keeps explaining the feeling it produced. Past this it is
    // history rather than the reason she feels something now, and repeating it would
    // have her narrate an event that has stopped mattering.
    static constexpr std::chrono::minutes causeLifetime{10};
    void SetMood(const MoodState& mood);
    void Reset();

    // The compatibility bridge.
    //
    // Speech, reflex, curiosity and presentation consume the projection of this
    // state. The prompt retains the full vector. The legacy classifier remains a
    // comparison evaluator; it cannot replace a calm canonical vector.
    [[nodiscard]] runtime::AffectSnapshot ToAffectSnapshot() const;

    // Mapping used by the bridge, exposed for testing. Every emotion resolves to some
    // legacy state; the projection is lossy by nature and that is acceptable, because
    // the full vector remains available to anything that wants it.
    [[nodiscard]] static runtime::AffectState ToAffectState(emotion::Emotion emotion);

    [[nodiscard]] std::string ModelName() const;

private:
    [[nodiscard]] runtime::AffectSnapshot ProjectAffect() const;
    std::optional<AppraisalOutcome> Appraise(
        const Stimulus& stimulus,
        const identity::DevelopmentState& development,
        const identity::RelationshipState* relationship,
        std::vector<RelevantMemory> memories);
    mutable std::mutex mutex;
    std::unique_ptr<IEmotionModel> model;
    EmotionVector emotion;
    MoodState mood;
    MoodController moodController;
    std::chrono::steady_clock::time_point lastConversation = std::chrono::steady_clock::now();
    bool quietConversationObserved = false;
    // The description of the stimulus that last actually moved her, not the whole
    // explanation: the packet already states which emotions it produced, and saying
    // so twice would let the two descriptions disagree.
    std::string lastCause;
    std::chrono::steady_clock::time_point lastCauseAt{};
};

} // namespace revia::emotion
