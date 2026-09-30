#pragma once

#include "Emotion/appraisalContext.h"
#include "Emotion/emotionTypes.h"
#include "Emotion/stimulus.h"

#include <memory>
#include <string>

namespace revia::emotion
{

// Evaluates only the supplied stimulus/context and returns an emotion delta.
// An event cannot replace the complete emotion state.
class IEmotionModel
{
public:
    virtual ~IEmotionModel() = default;

    [[nodiscard]] virtual EmotionVector Evaluate(const Stimulus& stimulus, const AppraisalContext& context) const = 0;

    // Recorded alongside logged appraisals so training data says which model produced a
    // target, and so a regression can be attributed rather than guessed at.
    [[nodiscard]] virtual std::string Name() const = 0;
};

// Deterministic appraisal remains the fallback, training bootstrap and comparison baseline.
// Personality is a separate multiplier rather than part of model weights.
class RuleEmotionModel final : public IEmotionModel
{
public:
    [[nodiscard]] EmotionVector Evaluate(const Stimulus& stimulus, const AppraisalContext& context) const override;

    [[nodiscard]] std::string Name() const override { return "rule-v2"; }

    // Exposed for testing and for training-data generation: the raw appraisal response
    // before personality and mood scale it. Comparing this against the final delta is
    // how the influence of character can be measured rather than asserted.
    [[nodiscard]] static EmotionVector RawResponse(const Stimulus& stimulus, const AppraisalContext& context);
};

// Chooses the model to use. Falls back to the rule model whenever a neural model is
// absent, has failed to load, or has been disabled, so there is never a state in which
// Revia has no emotional response available at all.
[[nodiscard]] std::unique_ptr<IEmotionModel> MakeDefaultEmotionModel();

} // namespace revia::emotion
