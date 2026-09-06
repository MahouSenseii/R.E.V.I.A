#pragma once

#include "Runtime/affectTypes.h"

#include <mutex>
#include <string>

namespace revia::intelligence
{

struct HumanizationState
{
    float moodPersistence = 0.65F;
    float curiosity = 0.55F;
    float socialEnergy = 0.6F;
    float irritation = 0.0F;
    float confidence = 0.62F;
    float familiarity = 0.25F;
    float talkativeness = 0.5F;
    float playfulness = 0.55F;
    std::string currentInterest;
    std::string unresolvedThought;

    // Comparison inputs for the legacy evaluator. Active appraisal reads the actual
    // relationship and mood owners, never these independently evolving metrics.
    [[nodiscard]] runtime::SocialContext Social() const
    {
        return {familiarity, irritation, socialEnergy, confidence};
    }
};

// Holds one social state for all model tiers, and renders none of it.
//
// This deliberately has no prompt output. It used to write a numeric row of curiosity,
// confidence, playfulness, talkativeness, social energy, familiarity, and irritation
// straight into the system prompt, alongside the state packet's prose description of the
// same traits from DevelopmentState, RelationshipState, and the emotion vector. Two
// descriptions of one personality, moving independently, one of them telemetry.
//
// The old numeric social reading remains only for evaluator comparison. Current interest
// and the generic unresolved outcome reach the model through ReviaStatePacket.
class HumanizationController
{
public:
    void ObserveInput(const std::string& input, const runtime::AffectSnapshot& affect);
    void ObserveOutcome(bool succeeded, const runtime::AffectSnapshot& affect);
    [[nodiscard]] HumanizationState Current() const;

private:
    mutable std::mutex mutex;
    HumanizationState state;
};

} // namespace revia::intelligence
