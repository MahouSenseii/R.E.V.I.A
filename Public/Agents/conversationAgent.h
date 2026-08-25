#pragma once

#include "Agents/conversationStylePolicy.h"
#include "Agents/responseFilter.h"
#include "Core/messageRouter.h"
#include "Intelligence/intelligenceTypes.h"

namespace revia::agents
{

class ConversationAgent
{
public:
    // onDelta forwards generated text as it arrives, so the caller can start speaking
    // the first sentence while the rest is still being produced.
    responseOutput Execute(
        const messageRouter& router,
        const std::string& input,
        const std::vector<conversationMessage>& context,
        const responseFilterSettings& filterSettings,
        const ResponseFilterContext& filterContext,
        std::stop_token stopToken = {},
        messageRouter::DeltaHandler onDelta = {},
        const revia::intelligence::IntelligenceDecision& decision = {}) const;

private:
    ConversationStylePolicy stylePolicy;
    ResponseFilter responseFilter;
};

} // namespace revia::agents
