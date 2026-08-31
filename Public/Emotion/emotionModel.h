#pragma once

#include "Emotion/appraisalContext.h"
#include "Emotion/emotionTypes.h"
#include "Emotion/stimulus.h"

#include <memory>
#include <string>

namespace revia::emotion
{

// What an emotion model must do, and nothing more.
//
// The interface exists so a trained network can replace the arithmetic below without
// anything upstream noticing. Both implementations answer the same question -- given
// this event in this context, what changes about how she feels -- and neither is allowed
// to reach outside the context it was handed.
//
// Evaluate returns a DELTA, not a new state. Returning an absolute state would let one
// event erase everything else being felt, which is the failure the vector was introduced
// to fix.
class IEmotionModel
{
public:
    virtual ~IEmotionModel() = default;

    [[nodiscard]] virtual EmotionVector Evaluate(
        const Stimulus& stimulus,
        const AppraisalContext& context) const = 0;

    // Recorded alongside logged appraisals so training data says which model produced a
    // target, and so a regression can be attributed rather than guessed at.
    [[nodiscard]] virtual std::string Name() const = 0;
};

// Deterministic appraisal arithmetic. The fallback, the bootstrap, and the baseline.
//
// This is not a placeholder to be deleted once a network exists. It stays as the answer
// when no model is loaded, as the source of initial training targets, and as the thing
// a trained model is compared against -- a network that cannot beat this is not worth
// the inference cost.
//
// Personality is applied as a separate multiplier at the end rather than being folded
// into the coefficients. That separation is deliberate: it keeps who she is out of the
// weights, so a trained model cannot quietly become the only place her character lives.
class RuleEmotionModel final : public IEmotionModel
{
public:
    [[nodiscard]] EmotionVector Evaluate(
        const Stimulus& stimulus,
        const AppraisalContext& context) const override;

    [[nodiscard]] std::string Name() const override { return "rule-v1"; }

    // Exposed for testing and for training-data generation: the raw appraisal response
    // before personality and mood scale it. Comparing this against the final delta is
    // how the influence of character can be measured rather than asserted.
    [[nodiscard]] static EmotionVector RawResponse(
        const Stimulus& stimulus,
        const AppraisalContext& context);
};

// Chooses the model to use. Falls back to the rule model whenever a neural model is
// absent, has failed to load, or has been disabled, so there is never a state in which
// Revia has no emotional response available at all.
[[nodiscard]] std::unique_ptr<IEmotionModel> MakeDefaultEmotionModel();

} // namespace revia::emotion
