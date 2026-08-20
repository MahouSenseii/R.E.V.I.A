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
    responseOutput output =
        router.RouteMessage(input, context, stopToken, std::move(onDelta));
    if (output.bSuccess)
    {
        output.response = stylePolicy.RefineReply(input, context, output.response);
    }
    return output;
}

} // namespace revia::agents
