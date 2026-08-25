#pragma once

#include "Agents/conversationAgent.h"
#include "Agents/memoryAgent.h"
#include "Intelligence/intelligenceTypes.h"

#include <cstdint>

namespace revia::agents
{

struct TurnAgentResult
{
    responseOutput response;
    bool memoryQueued = false;
};

class TurnCoordinator
{
public:
    TurnAgentResult Execute(
        const messageRouter& router,
        const std::string& input,
        const std::vector<conversationMessage>& context,
        const responseFilterSettings& filterSettings,
        const ResponseFilterContext& filterContext,
        bool evaluateMemory,
        std::uint64_t turnId = 0,
        std::stop_token stopToken = {},
        messageRouter::DeltaHandler onDelta = {},
        const revia::intelligence::IntelligenceDecision& decision = {}) const;
    std::vector<MemoryAgentEvent> DrainMemoryEvents();
    void SubmitLearnedFinding(
        const messageRouter& router,
        memoryDecision decision,
        std::uint64_t turnId = 0);
    void BackfillMemoryEmbeddings(
        const messageRouter& router,
        const std::string& embeddingModel);
    void Stop();

private:
    ConversationAgent conversationAgent;
    mutable MemoryAgent memoryAgent;
};

} // namespace revia::agents
