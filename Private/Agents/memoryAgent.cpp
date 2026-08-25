#include "Agents/memoryAgent.h"

#include <chrono>
#include <utility>

namespace revia::agents
{

MemoryAgent::MemoryAgent() : worker([this](const std::stop_token stopToken)
{
    Run(stopToken);
}) {}

MemoryAgent::~MemoryAgent()
{
    Stop();
}

void MemoryAgent::Submit(
    const messageRouter& router,
    std::string input,
    std::string assistantResponse,
    const std::uint64_t turnId)
{
    if (input.empty() || worker.get_stop_token().stop_requested())
    {
        return;
    }

    {
        std::lock_guard lock(mutex);
        Task task;
        task.router = &router;
        task.input = std::move(input);
        task.assistantResponse = std::move(assistantResponse);
        task.turnId = turnId;
        tasks.push_front(std::move(task));
    }
    taskAvailable.notify_one();
}

void MemoryAgent::SubmitLearnedFinding(
    const messageRouter& router,
    memoryDecision decision,
    const std::uint64_t turnId)
{
    if (!decision.bSuccess || !decision.bShouldRemember || decision.summary.empty() ||
        worker.get_stop_token().stop_requested())
    {
        return;
    }
    Task task;
    task.router = &router;
    task.input = decision.summary;
    task.learnedDecision = std::move(decision);
    task.hasLearnedDecision = true;
    task.turnId = turnId;
    {
        std::lock_guard lock(mutex);
        tasks.push_back(std::move(task));
    }
    taskAvailable.notify_one();
}

void MemoryAgent::SubmitEmbeddingBackfill(
    const messageRouter& router,
    const std::string& embeddingModel)
{
    if (embeddingModel.empty() || worker.get_stop_token().stop_requested())
    {
        return;
    }

    const std::vector<memoryEntry> missing =
        memory.LoadMissingEmbeddings(embeddingModel, 25);
    if (missing.empty())
    {
        return;
    }

    {
        std::lock_guard lock(mutex);
        for (const memoryEntry& entry : missing)
        {
            Task task;
            task.router = &router;
            task.input = entry.summary;
            task.memoryId = entry.id;
            tasks.push_back(std::move(task));
        }
    }
    taskAvailable.notify_one();
}

std::vector<MemoryAgentEvent> MemoryAgent::DrainEvents()
{
    std::lock_guard lock(mutex);
    std::vector<MemoryAgentEvent> drained;
    drained.swap(events);
    return drained;
}

void MemoryAgent::Stop()
{
    if (!worker.joinable())
    {
        return;
    }

    worker.request_stop();
    taskAvailable.notify_all();
    worker.join();
}

void MemoryAgent::Run(const std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        Task task;
        {
            std::unique_lock lock(mutex);
            taskAvailable.wait(lock, stopToken, [&]()
            {
                return !tasks.empty();
            });
            if (stopToken.stop_requested())
            {
                tasks.clear();
                return;
            }

            task = std::move(tasks.front());
            tasks.pop_front();
        }

        if (!task.memoryId.empty())
        {
            const embeddingOutput embedding =
                task.router->EmbedMemory(task.input, stopToken);
            if (stopToken.stop_requested())
            {
                return;
            }
            MemoryAgentEvent event;
            event.operation = "memory_backfill";
            event.decision.bSuccess = embedding.bSuccess;
            event.decision.reason = embedding.reason;
            event.decision.timings.push_back({
                "memory_document_embedding",
                embedding.elapsedMilliseconds});
            if (embedding.bSuccess)
            {
                const auto saveStarted = std::chrono::steady_clock::now();
                event.saveSucceeded = memory.SaveEmbedding(
                    task.memoryId,
                    embedding.model,
                    embedding.values);
                event.decision.timings.push_back({
                    "memory_db_save",
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - saveStarted).count()});
            }
            {
                std::lock_guard lock(mutex);
                events.push_back(std::move(event));
            }
            continue;
        }

        MemoryAgentEvent event;
        event.turnId = task.turnId;
        if (task.hasLearnedDecision)
        {
            event.operation = "autonomous_learning";
            event.decision = std::move(task.learnedDecision);
            const embeddingOutput embedding = task.router->EmbedMemory(
                event.decision.summary, stopToken);
            event.decision.timings.push_back({
                "autonomous_learning_embedding", embedding.elapsedMilliseconds});
            if (embedding.bSuccess)
            {
                event.decision.embedding = embedding.values;
                event.decision.embeddingModel = embedding.model;
            }
        }
        else
        {
            event.decision = task.router->EvaluateMemory(
                task.input, task.assistantResponse, stopToken);
        }
        if (stopToken.stop_requested())
        {
            return;
        }

        if (event.decision.bSuccess && event.decision.bShouldRemember)
        {
            const auto saveStarted = std::chrono::steady_clock::now();
            event.saveSucceeded = memory.SaveAutomaticMemory(event.decision, event.wasAdded);
            event.decision.timings.push_back({
                "memory_db_save",
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - saveStarted).count()});
        }

        {
            std::lock_guard lock(mutex);
            events.push_back(std::move(event));
        }
    }
}

} // namespace revia::agents
