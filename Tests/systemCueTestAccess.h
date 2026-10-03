#pragma once

#include "Speech/qwenTtsClient.h"
#include "Speech/qwenTtsPool.h"

#include <functional>
#include <mutex>
#include <vector>

namespace revia::speech
{

struct SystemCueTestAccess
{
    static std::vector<VoiceWorkerState> WorkerEstimates(QwenTtsPool& pool)
    {
        std::lock_guard lock(pool.mutex);
        std::vector<VoiceWorkerState> states;
        for (const auto& worker : pool.workers)
            states.push_back({worker.busy, worker.fixedOverheadMilliseconds, worker.millisecondsPerCharacter, worker.completed});
        return states;
    }
    static void WithClientLock(QwenTtsPool& pool, const std::function<void()>& action)
    {
        QwenTtsClient* client;
        {
            std::lock_guard lock(pool.mutex);
            client = pool.workers.front().client.get();
        }
        std::lock_guard lock(client->mutex);
        action();
    }
    static void WithPoolLock(QwenTtsPool& pool, const std::function<void()>& action)
    {
        std::lock_guard lock(pool.mutex);
        action();
    }
};

} // namespace revia::speech
