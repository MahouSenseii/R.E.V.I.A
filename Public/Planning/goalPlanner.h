#pragma once

#include "Goals/goalTypes.h"

#include <string>
#include <vector>

namespace revia::planning
{

struct ParsedGoal
{
    bool succeeded = false;
    goals::Goal goal;
    std::string error;
};

// succeeded means usable model output, including completion without a step.
// error also carries the reason a finished run stopped.
struct ParsedNextStep
{
    bool succeeded = false;
    bool finished = false;
    goals::GoalStep step;
    std::string error;
};

// Parses structure without executing or evaluating authority. GoalRunner validates;
// the caller supplies configured scope, never model-authored scope.
class GoalPlanner
{
public:
    [[nodiscard]] static ParsedGoal ParseJson(const std::string& input);

    // The system prompt describing the plan contract to the model.
    [[nodiscard]] static std::string PlannerPrompt();

    // Iterative planning uses observed attempts and can report completion.
    [[nodiscard]] static std::string NextStepPrompt();
    [[nodiscard]] static std::string NextStepSchema(const std::string& goalContext = {});

    // Subgoal grammar is separate from next-step action grammar.
    // Candidate names constrain targets to observed controls when available.
    [[nodiscard]] static std::string ComputerSubgoalSchema(const std::vector<std::string>& candidateNames,
        const std::vector<std::string>& containerNames);

    // Preserves step, completion, and undecided outcomes; caller maps them to runner types.
    [[nodiscard]] static ParsedNextStep ParseNextStep(const std::string& input);

    // Hard ceiling on plan length, applied before anything is executed. A model that
    // returns a hundred steps is malfunctioning, and the goal budget alone would not
    // catch it until execution was already underway.
    static constexpr std::size_t MaximumSteps = 12;
};

} // namespace revia::planning
