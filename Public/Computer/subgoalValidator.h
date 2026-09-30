#pragma once

#include "Computer/computerSubgoal.h"
#include "Computer/payloadVault.h"

namespace revia::computer
{

// Hard proposal bounds checked before interpretation.
inline constexpr std::size_t MaximumDescriptorField = 240;
inline constexpr std::size_t MaximumSubgoalDescription = 400;
inline constexpr std::size_t MaximumSubgoalPostconditions = 4;
inline constexpr std::size_t MaximumPayloadLength = 4096;

// Runtime-provided authority facts; proposals cannot supply origin, scope, or budget.
struct SubgoalContext
{
    std::string goalId;
    RequestOrigin origin = RequestOrigin::Unknown;
    actions::CapabilitySettings scope;
    std::uint32_t actionsLeft = 0;
    std::uint32_t retriesLeft = 0;
    // Whether this task exists to place content that has not been placed and verified
    // yet. A fact about the run, read from the goal's own record, and the reason a
    // subgoal proposing to press Send is refused while the box is still empty.
    bool contentPending = false;
};

// Validates supported progress against existing task authority, never the visible screen.
// Returns a narrower stamped subgoal or refusal; never widens scope.
[[nodiscard]] SubgoalValidation ValidateSubgoal(const ComputerSubgoal& proposed, const SubgoalContext& context, const PayloadVault& vault);

// Recognizes explicit submission names for placement ordering.
// Unrecognized names still pass through the separate consequence gate.
[[nodiscard]] bool NamesASubmission(const std::string& control);

// Whether an application is one the goal's scope already approves.
//
// Exposed because the routine policy asks the same question before proposing a launch,
// and two spellings of "is this allowed" is how they come to disagree.
[[nodiscard]] bool ScopeApprovesApplication(const actions::CapabilitySettings& scope, const std::string& application);

} // namespace revia::computer
