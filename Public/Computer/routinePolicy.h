#pragma once

#include "Computer/computerPolicy.h"
#include "Computer/targetMatch.h"
#include "Computer/computerSubgoal.h"
#include "Computer/payloadVault.h"

#include <cstdint>
#include <string>
#include <vector>

namespace revia::computer

{

// Deterministic bounded policy; ambiguous, stale, withheld, or unsupported cases
// abstain and route to Main. Proposed actions retain all execution checks.
class RoutineComputerPolicy final : public IComputerPolicy
{
public:
    // Borrows the vault only to validate references; never reads payload values.
    // The runtime supplies originals after authorization.
    explicit RoutineComputerPolicy(const PayloadVault& payloadVault);

    [[nodiscard]] std::string Name() const override { return "routine"; }
    // Always. It needs no artifact, no runtime and no backend, which is exactly why it
    // is the fallback that cannot itself fail to load.
    [[nodiscard]] bool IsAvailable() const override { return true; }

    [[nodiscard]] ComputerDecision Decide(const ComputerTaskContext& context, std::stop_token stopToken) override;

    // Accepts only validated subgoals at task boundaries; without one, always abstains.
    void SetSubgoal(ComputerSubgoal subgoal);
    void ClearSubgoal();
    [[nodiscard]] const ComputerSubgoal& Subgoal() const { return activeSubgoal; }

    // Reports coverage and avoids counting predictable abstentions as costly escalations.
    [[nodiscard]] bool CoversIntent(SubgoalIntent intent) const;

private:
    // Uses the shared target matcher so policy and runtime resolution agree.
    [[nodiscard]] TargetMatch FindTarget(const ComputerTaskContext& context, TargetAffordance affordance) const;

    const PayloadVault* vault = nullptr;
    ComputerSubgoal activeSubgoal;
};

} // namespace revia::computer
