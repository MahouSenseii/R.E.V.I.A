#include "Runtime/reviaSession.h"

namespace revia::runtime
{
autonomy::ActivityOutcome ReviaSession::ExecuteComputer(
    const autonomy::Activity& activity, const autonomy::ActivityDecision& decision,
    const std::stop_token stopToken)
{
    autonomy::ActivityOutcome outcome;
    auto parsed = actionRuntime.ParseJson(decision.operation);
    if (!parsed.succeeded)
    {
        outcome.status = autonomy::ActivityStatus::Cancelled;
        outcome.summary = "The PC activity was not a usable action: " + parsed.error;
        return outcome;
    }
    auto& request = parsed.request;
    // These activities may explore and make things in an approved scope. Moving or
    // deleting user work, web requests and arbitrary commands have separate owners.
    switch (request.type)
    {
        case actions::ActionType::ListDirectory:
        case actions::ActionType::ReadTextFile:
        case actions::ActionType::InspectWindow:
        case actions::ActionType::FocusWindow:
        case actions::ActionType::CreateDirectory:
        case actions::ActionType::CopyFile:
            break;
        default:
            outcome.status = autonomy::ActivityStatus::Cancelled;
            outcome.summary = "That PC action needs a user-directed task.";
            return outcome;
    }
    request.id = actions::NewActionId();
    request.requestedBy = "autonomous_activity/" + activity.id;
    request.dryRun = false;
    const auto policy = actionRuntime.Evaluate(request);
    if (policy.verdict != actions::PolicyVerdict::Allowed)
    {
        outcome.status = autonomy::ActivityStatus::Paused;
        outcome.summary = "The PC activity needs an approved scope: " + policy.reason;
        return outcome;
    }
    if (policy.risk != actions::RiskLevel::ReadOnly && GatherAutonomyCost().userIsBusy)
    {
        outcome.status = autonomy::ActivityStatus::Paused;
        outcome.summary = "Left the PC alone while the user was working.";
        return outcome;
    }
    if (stopToken.stop_requested() || ActivityWasInterrupted(activity.id))
    {
        outcome.status = autonomy::ActivityStatus::Interrupted;
        outcome.summary = "Set the PC activity aside for the user.";
        return outcome;
    }
    const auto result = actionRuntime.Execute(request, false, stopToken);
    outcome.status = result.Succeeded() ? autonomy::ActivityStatus::Completed :
        stopToken.stop_requested() ? autonomy::ActivityStatus::Interrupted : autonomy::ActivityStatus::Failed;
    outcome.satisfiedDrive = result.Succeeded();
    outcome.drive = autonomy::Drive::Exploration;
    outcome.summary = result.Message();
    // Local inspection stays in the local activity feed, never a search query or a
    // spontaneous spoken disclosure. Cap the display even if a file read is larger.
    RuntimeEvent observation;
    observation.kind = RuntimeEventKind::ComponentStatus;
    observation.component = "Autonomy";
    observation.phase = result.Succeeded() ? "Observed" : "Deferred";
    observation.message = outcome.summary;
    observation.detail = result.result.content.substr(0, 2400);
    eventBus.Publish(std::move(observation));
    return outcome;
}
}
