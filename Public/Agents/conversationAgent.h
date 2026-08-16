#pragma once

#include "Core/messageRouter.h"

namespace revia::agents
{

class ConversationAgent
{
public:
    responseOutput Execute(
        const messageRouter& router,
        const std::string& input,
        const std::vector<conversationMessage>& context,
        std::stop_token stopToken = {}) const;
};

} // namespace revia::agents
