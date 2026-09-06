#pragma once
#include "Agents/memoryAgent.h"

namespace revia::agents
{
struct MemoryAgentTestAccess
{
    static void FastBackfill(MemoryAgent& agent)
    {
        std::lock_guard lock(agent.mutex);
        agent.backfillBatchInterval = std::chrono::milliseconds(10);
        agent.backfillIdleInterval = std::chrono::milliseconds(120);
        agent.backfillRetryInterval = std::chrono::milliseconds(80);
        agent.backfillMaximumRetry = std::chrono::milliseconds(320);
    }
};
}
