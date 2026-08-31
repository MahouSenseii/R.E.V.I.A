#pragma once

#include "Emotion/appraisalContext.h"
#include "Emotion/emotionModel.h"
#include "Emotion/emotionTypes.h"
#include "Emotion/moodState.h"
#include "Emotion/stimulus.h"
#include "Identity/developmentState.h"
#include "Identity/relationshipState.h"
#include "Runtime/affectTypes.h"

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

    [[nodiscard]] EmotionVector Emotion() const;
    [[nodiscard]] MoodState Mood() const;
    void SetMood(const MoodState& mood);
    void Reset();

    // The compatibility bridge.
    //
    // Everything downstream -- the status badge, speech rate, the posture line in the
    // prompt -- currently consumes a single AffectSnapshot. Rather than change all of
    // them at once, the vector is projected onto the old shape, so the new system can be
    // adopted incrementally and the deterministic AffectController stays a working
    // fallback rather than becoming dead code.
    [[nodiscard]] runtime::AffectSnapshot ToAffectSnapshot() const;

    // Mapping used by the bridge, exposed for testing. Every emotion resolves to some
    // legacy state; the projection is lossy by nature and that is acceptable, because
    // the full vector remains available to anything that wants it.
    [[nodiscard]] static runtime::AffectState ToAffectState(emotion::Emotion emotion);

    [[nodiscard]] std::string ModelName() const;

private:
    mutable std::mutex mutex;
    std::unique_ptr<IEmotionModel> model;
    EmotionVector emotion;
    MoodState mood;
    MoodController moodController;
};

} // namespace revia::emotion
