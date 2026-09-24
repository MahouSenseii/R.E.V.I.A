#pragma once

#include "Agents/conversationStylePolicy.h"
#include "Agents/responseFilter.h"
#include "Core/messageRouter.h"
#include "Intelligence/intelligenceTypes.h"
#include "LLM/privateMemoryAccess.h"

namespace revia::agents
{

class ConversationAgent
{
public:
    // onDelta receives the completed, approved answer only after all enabled filters.
    // Raw model chunks never reach a speech/display callback. Cancellation or failed
    // generation delivers nothing; callers may fragment the approved text for speech.
    responseOutput Execute(
        const messageRouter& router,
        const std::string& input,
        const std::vector<conversationMessage>& context,
        const responseFilterSettings& filterSettings,
        const ResponseFilterContext& filterContext,
        std::stop_token stopToken = {},
        messageRouter::DeltaHandler onDelta = {},
        const revia::intelligence::IntelligenceDecision& decision = {},
        llm::PrivateMemoryAccess memoryAccess = llm::PrivateMemoryAccess::ProfileSetting) const;

private:
    ConversationStylePolicy stylePolicy;
    ResponseFilter responseFilter;
};

} // namespace revia::agents
