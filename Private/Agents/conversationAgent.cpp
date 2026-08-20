#include "Agents/conversationAgent.h"

namespace revia::agents
{

responseOutput ConversationAgent::Execute(
    const messageRouter& router,
    const std::string& input,
    const std::vector<conversationMessage>& context,
    const std::stop_token stopToken,
    messageRouter::DeltaHandler onDelta) const
{
    return router.RouteMessage(input, context, stopToken, std::move(onDelta));
}

} // namespace revia::agents
