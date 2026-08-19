#pragma once

#include "Goals/goalTypes.h"

#include <string>

namespace revia::planning
{

struct ParsedGoal
{
    bool succeeded = false;
    goals::Goal goal;
    std::string error;
};

// Turns a planner response into a Goal.
//
// This is the authoring half of Stage 4. It performs no policy evaluation and
// executes nothing: it decodes the plan and rejects anything structurally unusable,
// then GoalRunner::Validate applies the verification rules and the capability policy
// applies the authority rules. The parser deliberately does not read a capability
// scope out of the plan, because a goal that chose its own scope could widen its own
// authority; the caller supplies the scope from configured policy instead.
class GoalPlanner
{
public:
    [[nodiscard]] static ParsedGoal ParseJson(const std::string& input);

    // The system prompt describing the plan contract to the model.
    [[nodiscard]] static std::string PlannerPrompt();

    // Hard ceiling on plan length, applied before anything is executed. A model that
    // returns a hundred steps is malfunctioning, and the goal budget alone would not
    // catch it until execution was already underway.
    static constexpr std::size_t MaximumSteps = 12;
};

} // namespace revia::planning
