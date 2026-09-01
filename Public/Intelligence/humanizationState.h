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

// Holds one social state for all model tiers, and renders none of it.
//
// This deliberately has no prompt output. It used to write a numeric row of curiosity,
// confidence, playfulness, talkativeness, social energy, familiarity, and irritation
// straight into the system prompt, alongside the state packet's prose description of the
// same traits from DevelopmentState, RelationshipState, and the emotion vector. Two
// descriptions of one personality, moving independently, one of them telemetry.
//
// What survives here is the part nothing else owns: the short-horizon social reading the
// affect classifier needs, and the unresolved thought an outcome leaves behind. Both
// reach the model through ReviaStatePacket like everything else.
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
