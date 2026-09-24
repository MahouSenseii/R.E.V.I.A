#include "Computer/subgoalPlanner.h"

#include "Planning/goalPlanner.h"

#include <algorithm>

#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

namespace revia::computer
{

namespace
{

// Same stripper the existing planner uses, for the same reason: a model asked for JSON
// will sometimes wrap it in a fence, and refusing that would refuse a correct answer
// over its packaging.
std::string StripCodeFence(const std::string& input)
{
    const std::size_t open = input.find("```");
    if (open == std::string::npos) return input;
    const std::size_t afterFence = input.find('\n', open);
    if (afterFence == std::string::npos) return input;
    const std::size_t close = input.find("```", afterFence);
    if (close == std::string::npos) return input.substr(afterFence + 1);
    return input.substr(afterFence + 1, close - afterFence - 1);
}

ParsedSubgoal Failure(std::string message)
{
    ParsedSubgoal parsed;
    parsed.error = std::move(message);
    return parsed;
}

std::string Field(const nlohmann::json& data, const char* name)
{
    if (!data.contains(name) || !data[name].is_string()) return {};
    return data[name].get<std::string>();
}

} // namespace

SubgoalRequest FormatSubgoalRequest(
    const std::string& task,
    const ComputerTaskContext& context,
    const PayloadReference& availablePayload)
{
    // What is on screen, bounded, and what may be referenced. The payload appears here
    // by reference and by *kind* and never by value: describing it as "a message of 15
    // characters" is enough for a model to decide where it belongs, and showing it the
    // message would put the user's words into a prompt for no gain.
    nlohmann::json candidates = nlohmann::json::array();
    std::vector<std::string> candidateNames;
    std::vector<std::string> containerNames;
    for (const ObservedCandidate& candidate : context.observation.candidates)
    {
        nlohmann::json entry = {
            {"role", candidate.role},
            {"may_invoke", candidate.mayInvoke},
            {"may_edit", candidate.maySetText || candidate.mayType}};
        if (!candidate.name.empty())
        {
            entry["name"] = candidate.name;
            if (std::find(candidateNames.begin(), candidateNames.end(), candidate.name) ==
                candidateNames.end())
            {
                candidateNames.push_back(candidate.name);
            }
        }
        else
        {
            // Said plainly, because it changes what the model has to do. A field with
            // no name is reachable only by the panel it sits in, and a model that was
            // not told that will invent a name and get nothing.
            entry["name"] = nullptr;
            entry["unnamed"] = true;
        }
        if (!candidate.inferredLabel.empty())
        {
            entry["inferred_label"] = candidate.inferredLabel;
            if (std::find(candidateNames.begin(), candidateNames.end(),
                    candidate.inferredLabel) == candidateNames.end())
            {
                candidateNames.push_back(candidate.inferredLabel);
            }
        }
        if (!candidate.container.empty())
        {
            entry["in"] = candidate.container;
            if (std::find(containerNames.begin(), containerNames.end(),
                    candidate.container) == containerNames.end())
            {
                containerNames.push_back(candidate.container);
            }
        }
        candidates.push_back(std::move(entry));
    }

    nlohmann::json payload = nullptr;
    if (availablePayload.Valid())
    {
        payload = {
            {"id", availablePayload.id},
            {"kind", availablePayload.kind},
            {"length", availablePayload.length},
            // Where it came from. A model choosing a destination benefits from knowing
            // whether these are the user's own words or something she composed, and
            // neither answer reveals a character of the value.
            {"provenance", ToString(availablePayload.provenance)}};
        if (!context.preparedContent.destination.empty())
        {
            payload["destination"] = context.preparedContent.destination;
        }
    }

    SubgoalRequest request;
    request.instruction =
        "You decide the next bounded piece of local progress on the machine.\n"
        "Return exactly one JSON object and nothing else.\n"
        "\n"
        "Describe ONE narrow step, not a workflow. \"Open the conversation with the "
        "recipient already identified\" is a step. \"Send my sister a message\" is not.\n"
        "\n"
        "Fields:\n"
        "  intent       launch_application | focus_window | resolve_target |\n"
        "               interact_with_control | enter_payload | wait_for_state | escalate\n"
        "  description  one short sentence for the person watching\n"
        "  target       {application, window_title, name, role, container}\n"
        "  payload_id   only for enter_payload, and only an id you were given\n"
        "\n"
        "Rules.\n"
        "- Name the application on every intent except wait_for_state and escalate.\n"
        "- Use only names listed in candidates. Do not invent one.\n"
        "- A candidate marked \"unnamed\" has no name. Reach it by leaving name empty "
        "and giving its \"in\" value as container, together with its role.\n"
        "- Never write the content of a payload. Reference it by id; the runtime "
        "supplies the original after the action is authorized.\n"
        "- If the recipient, the target or the destination is ambiguous, answer "
        "escalate rather than choosing.\n"
        "- Use focus_window ONLY when \"foreground\" is not already the application you "
        "need. If it is already in front, that step is done -- choose the one that makes "
        "actual progress instead.\n"
        "- Look at what the previous steps already achieved. Do not repeat work that is "
        "finished.\n"
        "- When \"available_payload\" is not null and the task is to put that "
        "content somewhere, the intent is enter_payload and you must give its id "
        "as payload_id. resolve_target only finds a control and reports it; it "
        "puts nothing anywhere.\n"
        "- Finding a field, filling it, and sending what is in it are three different "
        "steps. A task to put content somewhere is finished when the content is in the "
        "field and has been read back. It does not authorize pressing Send, Submit, "
        "Post or Reply, and proposing one of those before the content is in the field "
        "will be refused.\n"
        "- \"destination\", when present, is the field the user named. Aim at it.";

    // What has already been done, bounded. Without it the planner proposes the same
    // subgoal on every iteration: it has no memory, the screen looks much the same
    // after a focus, and "the window is in front" reads as a reason to focus it.
    nlohmann::json done = nlohmann::json::array();
    for (const ComputerAttempt& attempt : context.recentAttempts)
    {
        done.push_back({
            {"step", attempt.description},
            {"action", actions::ToString(attempt.action)},
            {"worked", attempt.verified}});
    }

    request.situation = nlohmann::json({
        {"task", task},
        {"observation_available", context.observation.Available()},
        {"foreground", context.observation.screen.foregroundApplication},
        {"window_title", context.observation.screen.foregroundTitle},
        {"already_done", std::move(done)},
        {"candidates", std::move(candidates)},
        {"approved_applications", context.scope.approvedApplications},
        {"actions_left", context.actionsLeft},
        {"available_payload", std::move(payload)}}).dump();

    request.schema = planning::GoalPlanner::ComputerSubgoalSchema(
        candidateNames, containerNames);
    return request;
}

ParsedSubgoal ParseSubgoal(const std::string& answer)
{
    // Bounded before it is read. An answer this large has stopped being a subgoal, and
    // the cheapest moment to refuse malformed output is before it is interpreted.
    if (answer.size() > MaximumSubgoalAnswerBytes)
    {
        return Failure("The subgoal answer was larger than a subgoal can be.");
    }

    nlohmann::json data;
    try
    {
        data = nlohmann::json::parse(StripCodeFence(answer));
    }
    catch (const std::exception& error)
    {
        return Failure(std::string("Invalid subgoal JSON: ") + error.what());
    }
    if (!data.is_object())
    {
        return Failure("The subgoal was not a JSON object.");
    }

    ParsedSubgoal parsed;
    parsed.proposed.id = NewSubgoalId();
    parsed.proposed.schemaVersion = CurrentSubgoalSchema;
    parsed.proposed.intent = SubgoalIntentFromString(Field(data, "intent"));
    if (parsed.proposed.intent == SubgoalIntent::Unspecified)
    {
        // Named nothing this build implements. Refused by that name rather than mapped
        // to a nearest neighbour, because a contract partly understood is not one.
        return Failure("The subgoal named no intent this build can act on.");
    }
    parsed.proposed.description = Field(data, "description");

    if (data.contains("target") && data["target"].is_object())
    {
        const nlohmann::json& target = data["target"];
        parsed.proposed.target.application = Field(target, "application");
        parsed.proposed.target.windowTitle = Field(target, "window_title");
        parsed.proposed.target.name = Field(target, "name");
        parsed.proposed.target.role = Field(target, "role");
        parsed.proposed.target.container = Field(target, "container");
    }

    // A payload is referenced, never carried. If the model wrote a value instead of an
    // id the value is simply not read: there is no field here that would accept it.
    const std::string payloadId = Field(data, "payload_id");
    if (!payloadId.empty())
    {
        parsed.proposed.payload.id = payloadId;
    }

    parsed.succeeded = true;
    return parsed;
}

} // namespace revia::computer
