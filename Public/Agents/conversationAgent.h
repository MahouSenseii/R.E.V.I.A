#pragma once

#include "Core/messageRouter.h"

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
        std::stop_token stopToken = {},
        messageRouter::DeltaHandler onDelta = {}) const;
};

} // namespace revia::agents
