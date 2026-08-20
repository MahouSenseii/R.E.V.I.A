#pragma once

#include "Agents/conversationAgent.h"
#include "Agents/memoryAgent.h"

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
        bool evaluateMemory,
        std::uint64_t turnId = 0,
        std::stop_token stopToken = {},
        messageRouter::DeltaHandler onDelta = {}) const;
    std::vector<MemoryAgentEvent> DrainMemoryEvents();
    void BackfillMemoryEmbeddings(
        const messageRouter& router,
        const std::string& embeddingModel);
    void Stop();

private:
    ConversationAgent conversationAgent;
    mutable MemoryAgent memoryAgent;
};

} // namespace revia::agents
