#pragma once

#include "Computer/computerSubgoal.h"
#include "Computer/payloadVault.h"

namespace revia::computer
{

// Bounds on what a proposal may contain, checked before anything reads it closely.
//
// Not tuning knobs. A descriptor field long enough to be a paragraph is not a
// descriptor, and a proposal carrying more postconditions than a subgoal can have is
// not a subgoal -- both are the shape of output that has stopped following the contract,
// and the cheapest moment to refuse them is before they are interpreted.
inline constexpr std::size_t MaximumDescriptorField = 240;
inline constexpr std::size_t MaximumSubgoalDescription = 400;
inline constexpr std::size_t MaximumSubgoalPostconditions = 4;
inline constexpr std::size_t MaximumPayloadLength = 4096;

// What the runtime attaches to a proposal it accepts.
//
// Passed in rather than read from the proposal, because every field here is one a model
// would otherwise be asserting about its own authority. Origin, scope and budget are
// facts about the run; a proposal that contained them would be a proposal that could
// change them.
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

// The gate between what Main proposed and what anything is allowed to act on.
//
// It answers one question -- does this proposal describe a piece of local progress the
// runtime supports, inside the authority this run already has? -- and answers it without
// consulting the screen. That is deliberate: scope is a property of what the user
// authorized, not of what happens to be in front, and checking authority against the
// desktop is how a window that appears mid-run becomes permission to use it.
//
// Nothing here widens anything. Every outcome is either a narrower, stamped subgoal or
// a refusal with a reason.
[[nodiscard]] SubgoalValidation ValidateSubgoal(
    const ComputerSubgoal& proposed,
    const SubgoalContext& context,
    const PayloadVault& vault);

// Whether a control's name describes submitting rather than editing.
//
// Deliberately a small, readable list rather than a classifier. It is used for one
// purpose -- refusing to submit before there is anything to submit -- and a rule that
// stops a task has to be one a person can read and predict. Anything it does not
// recognise is simply not refused here, and the consequence gate still has its own say
// about what an action would cause.
[[nodiscard]] bool NamesASubmission(const std::string& control);

// Whether an application is one the goal's scope already approves.
//
// Exposed because the routine policy asks the same question before proposing a launch,
// and two spellings of "is this allowed" is how they come to disagree.
[[nodiscard]] bool ScopeApprovesApplication(
    const actions::CapabilitySettings& scope, const std::string& application);

} // namespace revia::computer
