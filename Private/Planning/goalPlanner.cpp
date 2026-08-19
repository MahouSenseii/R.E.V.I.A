#include "Planning/goalPlanner.h"

#include "Planning/structuredActionParser.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace revia::planning
{

namespace
{

ParsedGoal Error(std::string message)
{
    ParsedGoal result;
    result.error = std::move(message);
    return result;
}

std::string StripCodeFence(const std::string& input)
{
    const std::size_t open = input.find("```");
    if (open == std::string::npos)
    {
        return input;
    }
    const std::size_t afterFence = input.find('\n', open);
    if (afterFence == std::string::npos)
    {
        return input;
    }
    const std::size_t close = input.find("```", afterFence);
    if (close == std::string::npos)
    {
        return input.substr(afterFence + 1);
    }
    return input.substr(afterFence + 1, close - afterFence - 1);
}

} // namespace

std::string GoalPlanner::PlannerPrompt()
{
    return
        "You are Revia's goal planner. Return exactly one JSON object and no markdown.\n"
        "Shape: {\"title\":\"short goal title\",\"steps\":[{\"description\":\"what this step "
        "does\",\"action\":{...},\"check\":{...},\"expected\":\"text the check output must "
        "contain\"}]}\n"
        "Every step needs all four fields. `action` performs the work; `check` observes the "
        "result and MUST be a read-only action (list_directory, read_text_file, or "
        "inspect_window); `expected` is a literal substring that will appear in the check "
        "output only if the action worked.\n"
        "Allowed action values are list_directory, read_text_file, create_directory, "
        "copy_file, move_file, rename_path, move_to_recycle_bin, inspect_window, "
        "focus_window, set_control_text, and invoke_control. Filesystem actions use an "
        "absolute Windows path in source or path; copy_file, move_file, and rename_path also "
        "require destination. Desktop actions require application (an exe name) and may use "
        "window_title; set_control_text requires control and value; invoke_control requires "
        "control.\n"
        "Plan the fewest steps that achieve the request, at most 12. Never emit shell "
        "commands, scripts, or explanations. If the request cannot be expressed as these "
        "actions, return {\"goal\":\"unknown\",\"reason\":\"brief reason\"}.";
}

ParsedGoal GoalPlanner::ParseJson(const std::string& input)
{
    nlohmann::json data;
    try
    {
        data = nlohmann::json::parse(StripCodeFence(input));
    }
    catch (const std::exception& error)
    {
        return Error(std::string("Invalid goal plan JSON: ") + error.what());
    }

    if (!data.is_object())
    {
        return Error("A goal plan must be a JSON object.");
    }
    if (data.contains("goal") && data["goal"].is_string() &&
        data["goal"].get<std::string>() == "unknown")
    {
        return Error("The planner could not express this as a goal: " +
            data.value("reason", "no reason given"));
    }
    if (!data.contains("steps") || !data["steps"].is_array())
    {
        return Error("A goal plan requires a steps array.");
    }

    const nlohmann::json& steps = data["steps"];
    if (steps.empty())
    {
        return Error("A goal plan requires at least one step.");
    }
    if (steps.size() > MaximumSteps)
    {
        return Error("A goal plan may contain at most " + std::to_string(MaximumSteps) +
            " steps; the planner returned " + std::to_string(steps.size()) + ".");
    }

    ParsedGoal result;
    result.goal.id = goals::NewGoalId();
    result.goal.title = data.value("title", "Untitled goal");
    result.goal.status = goals::GoalStatus::Planned;

    std::uint32_t ordinal = 0;
    for (const nlohmann::json& entry : steps)
    {
        const std::string position = "Step " + std::to_string(ordinal + 1);
        if (!entry.is_object())
        {
            return Error(position + " is not a JSON object.");
        }
        if (!entry.contains("action") || !entry.contains("check"))
        {
            return Error(position + " needs both an action and a check.");
        }

        const ParsedAction action = StructuredActionParser::ParseObject(entry["action"]);
        if (!action.succeeded)
        {
            return Error(position + " has an unusable action: " + action.error);
        }
        const ParsedAction check = StructuredActionParser::ParseObject(entry["check"]);
        if (!check.succeeded)
        {
            return Error(position + " has an unusable check: " + check.error);
        }

        goals::GoalStep step;
        step.id = goals::NewStepId();
        step.ordinal = ordinal;
        step.description = entry.value("description", result.goal.title);
        step.action = action.request;
        step.check = check.request;
        step.expected = entry.value("expected", "");
        // Left to GoalRunner::Validate rather than duplicated here: it already refuses a
        // step with no expectation or a check that is not read-only, and one rule with one
        // owner is what keeps the authoring and execution halves from disagreeing.
        result.goal.steps.push_back(std::move(step));
        ++ordinal;
    }

    result.succeeded = true;
    return result;
}

} // namespace revia::planning
