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

// One answer from the iterative planner.
//
// `succeeded` means the model produced a usable answer, not that there is work to do:
// a run that is finished succeeds and carries no step. `error` doubles as the reason a
// finished run gives, so the record says why it stopped either way.
struct ParsedNextStep
{
    bool succeeded = false;
    bool finished = false;
    goals::GoalStep step;
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

    // The same contract for one step at a time, for the iterative operator loop.
    //
    // Separate from PlannerPrompt because the questions differ: a plan is written before
    // anything is seen, while this is answered after every previous attempt and its
    // observed outcome. It therefore also has to be able to say the work is already
    // done, which a plan never says about itself.
    [[nodiscard]] static std::string NextStepPrompt();
    [[nodiscard]] static std::string NextStepSchema(const std::string& goalContext = {});

    // Reads one step decision. Distinguishes a step to take, nothing left to take, and
    // no usable answer, because the last two are opposite outcomes and collapsing them
    // would let "stuck" be recorded as "finished".
    //
    // Returns this rather than goals::NextStep so Planning does not have to include the
    // runner, and through it the action runtime and the policy. The caller translates.
    [[nodiscard]] static ParsedNextStep ParseNextStep(const std::string& input);

    // Hard ceiling on plan length, applied before anything is executed. A model that
    // returns a hundred steps is malfunctioning, and the goal budget alone would not
    // catch it until execution was already underway.
    static constexpr std::size_t MaximumSteps = 12;
};

} // namespace revia::planning
