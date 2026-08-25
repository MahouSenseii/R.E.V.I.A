#pragma once

#include "Core/memoryManager.h"
#include "Core/messageRouter.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace revia::agents
{

struct MemoryAgentEvent
{
    memoryDecision decision;
    std::uint64_t turnId = 0;
    std::string operation = "memory_evaluation";
    bool saveSucceeded = true;
    bool wasAdded = false;
};

class MemoryAgent
{
public:
    MemoryAgent();
    ~MemoryAgent();

    MemoryAgent(const MemoryAgent&) = delete;
    MemoryAgent& operator=(const MemoryAgent&) = delete;

    void Submit(
        const messageRouter& router,
        std::string input,
        std::string assistantResponse = "",
        std::uint64_t turnId = 0);
    // Stores one already-grounded, bounded finding without asking the conversation
    // classifier to reinterpret web text. Embedding still runs on the background lane.
    void SubmitLearnedFinding(
        const messageRouter& router,
        memoryDecision decision,
        std::uint64_t turnId = 0);
    void SubmitEmbeddingBackfill(
        const messageRouter& router,
        const std::string& embeddingModel);
    std::vector<MemoryAgentEvent> DrainEvents();
    void Stop();

private:
    struct Task
    {
        const messageRouter* router = nullptr;
        std::string input;
        std::string assistantResponse;
        std::string memoryId;
        memoryDecision learnedDecision;
        bool hasLearnedDecision = false;
        std::uint64_t turnId = 0;
    };

    void Run(std::stop_token stopToken);

    memoryManager memory;
    std::mutex mutex;
    std::condition_variable_any taskAvailable;
    std::deque<Task> tasks;
    std::vector<MemoryAgentEvent> events;
    std::jthread worker;
};

} // namespace revia::agents
