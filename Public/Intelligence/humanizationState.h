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

    // The subset the affect classifier needs to tell who is speaking and how the day has
    // gone. Returning a narrow struct rather than exposing the whole state keeps the
    // dependency one-directional: affect reads four numbers, not a social model.
    [[nodiscard]] runtime::SocialContext Social() const
    {
        return {familiarity, irritation, socialEnergy, confidence};
    }
};

// Holds one social state for all model tiers. Models receive a compact description of
// this state; they never own separate moods or relationships.
class HumanizationController
{
public:
    void ObserveInput(const std::string& input, const runtime::AffectSnapshot& affect);
    void ObserveOutcome(bool succeeded, const runtime::AffectSnapshot& affect);
    [[nodiscard]] HumanizationState Current() const;
    [[nodiscard]] std::string BuildPromptBlock() const;

private:
    mutable std::mutex mutex;
    HumanizationState state;
};

} // namespace revia::intelligence
