#include "Games/gameActionExecutor.h"

namespace revia::games
{

GameActionExecutor::GameActionExecutor(std::shared_ptr<NeuroGameServer> inputServer)
    : server(std::move(inputServer))
{
}

bool GameActionExecutor::Handles(const actions::ActionType type) const
{
    return type == actions::ActionType::GameAction;
}

actions::ActionResult GameActionExecutor::Execute(
    const actions::ActionRequest& request,
    const actions::PolicyDecision& decision)
{
    (void)decision;
    actions::ActionResult result;
    result.attempted = true;
    result.backend = "game:" + request.application;
    if (!server || !server->IsRunning())
    {
        result.message = "No game server is running.";
        return result;
    }
    std::string error;
    const std::string id = server->SendAction(request.application, request.value, request.arguments, error);
    if (id.empty())
    {
        result.message = error;
        return result;
    }
    const std::optional<ActionResultMessage> verdict = server->WaitForResult(id, ResultTimeout);
    if (!verdict)
    {
        result.message = "The game did not report on '" + request.value + "' in time.";
        return result;
    }
    result.succeeded = verdict->success;
    result.content = verdict->message;
    result.message = verdict->success
        ? "The game accepted '" + request.value + "'" + (verdict->message.empty() ? "." : ": " + verdict->message)
        : "The game rejected '" + request.value + "'" + (verdict->message.empty() ? "." : ": " + verdict->message);
    return result;
}

} // namespace revia::games
