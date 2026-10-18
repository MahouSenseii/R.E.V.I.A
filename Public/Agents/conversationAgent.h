#pragma once

#include "Agents/responseFilterSettings.h"
#include "Core/conversationMessage.h"
#include "LLM/responseTypes.h"
#include "Agents/conversationStylePolicy.h"
#include "Agents/responseFilter.h"
#include "Core/messageRouter.h"
#include "Intelligence/intelligenceTypes.h"
#include "LLM/privateMemoryAccess.h"

namespace revia::agents
{

class ToolLoop;

class ConversationAgent
{
public:
    // onDelta receives approved text only; raw model chunks never reach it. On an ordinary
    // conversation turn with no AI reviewer, each complete sentence arrives as soon as the
    // deterministic filters leave the reply so far unchanged, and the rest after the final
    // pass. A reply that must be reviewed or validated whole arrives once, after every
    // enabled filter. The deltas add up to the final reply unless the full pass changed
    // text already released; callers compare them with the returned response. Cancellation
    // or failed generation delivers nothing more.
    //
    // With an active `tools` loop the answer may take several model steps. Their text
    // passes the same gate as one reply, joined in order, and the final pass judges it
    // whole; a step that only asks for tools releases nothing the gate did not approve.
    responseOutput Execute(const messageRouter& router, const std::string& input, const std::vector<conversationMessage>& context,
        const responseFilterSettings& filterSettings, const ResponseFilterContext& filterContext, std::stop_token stopToken = {},
        messageRouter::DeltaHandler onDelta = {}, const revia::intelligence::IntelligenceDecision& decision = {},
        llm::PrivateMemoryAccess memoryAccess = llm::PrivateMemoryAccess::ProfileSetting, const ToolLoop* tools = nullptr) const;

private:
    ConversationStylePolicy stylePolicy;
    ResponseFilter responseFilter;
};

} // namespace revia::agents
