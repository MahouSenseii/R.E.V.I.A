#pragma once
#include "Memory/memoryTypes.h"
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

    static void SetEvaluator(MemoryAgent& agent, std::function<memoryDecision(const std::string&, const std::string&)> evaluator)
    {
        std::lock_guard lock(agent.mutex);
        agent.evaluateOverride = std::move(evaluator);
    }

    static void QueueEvent(MemoryAgent& agent, std::function<bool()> guard)
    {
        std::lock_guard lock(agent.mutex);
        MemoryAgentEvent event;
        event.decision.bSuccess = true;
        (void)agent.AdmitEventLocked(std::move(event), std::move(guard));
    }

    static void CaptureGuard(MemoryAgent& agent)
    {
        (void)agent.CaptureAdmission();
    }

    static std::size_t MarkDuplicates(MemoryAgent& agent, const std::string& id, const memory::MemoryScope& scope,
        std::function<bool()> admission = {})
    {
        MemoryAgent::Task task;
        task.scope = scope;
        task.admission = std::move(admission);
        return agent.MarkSemanticDuplicates(id, "fixture-embedding", task, agent.worker.get_stop_token());
    }
};
}
