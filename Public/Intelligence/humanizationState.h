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

// Shares conversational state across tiers. Legacy social metrics are used only by comparison evaluators.
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
