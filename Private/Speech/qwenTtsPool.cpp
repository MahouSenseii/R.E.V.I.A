#include "Speech/qwenTtsPool.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <limits>
#include <set>

namespace revia::speech
{

QwenTtsPool::~QwenTtsPool()
{
    Shutdown();
}

void QwenTtsPool::Configure(const speechSettings& settings)
{
    Shutdown();
    std::vector<std::string> devices = settings.qwenDevices;
    if (devices.empty()) devices.push_back(settings.qwenDevice);
    std::set<std::string> seen;
    const int maximum = std::max(1, settings.qwenMaxWorkers);
    std::lock_guard lock(mutex);
    shuttingDown = false;
    designSettings = settings;
    designSettings.qwenPort = std::min(65535, settings.qwenPort + 32);
    designSettings.qwenDevice = devices.front();
    designSettings.qwenDevices = {designSettings.qwenDevice};
    designClient = std::make_unique<QwenTtsClient>();
    designClient->Configure(designSettings);
    for (const std::string& device : devices)
    {
        if (device.empty() || !seen.insert(device).second ||
            workers.size() >= static_cast<std::size_t>(maximum))
        {
            continue;
        }
        speechSettings workerSettings = settings;
        workerSettings.qwenDevice = device;
        workerSettings.qwenDevices = {device};
        workerSettings.qwenPort = settings.qwenPort + static_cast<int>(workers.size());
        auto client = std::make_unique<QwenTtsClient>();
        client->Configure(workerSettings);
        Worker worker;
        worker.id = "voice-worker-" + std::to_string(workers.size());
        worker.device = device;
        worker.client = std::move(client);
        // The first worker is the resource planner's latency-first choice. New workers
        // begin with a small uncertainty penalty until real timings replace it.
        worker.fixedOverheadMilliseconds = 5000.0 + workers.size() * 1200.0;
        worker.millisecondsPerCharacter = 180.0 + workers.size() * 35.0;
        workers.push_back(std::move(worker));
    }
    if (workers.empty())
    {
        speechSettings workerSettings = settings;
        auto client = std::make_unique<QwenTtsClient>();
        client->Configure(workerSettings);
        Worker worker;
        worker.id = "voice-worker-0";
        worker.device = settings.qwenDevice;
        worker.client = std::move(client);
        workers.push_back(std::move(worker));
    }
}

std::size_t QwenTtsPool::WorkerCount() const
{
    std::lock_guard lock(mutex);
    return workers.size();
}

VoiceOperationResult QwenTtsPool::PrepareVoice(const VoicePreset& preset)
{
    std::vector<std::pair<std::string, QwenTtsClient*>> clients;
    {
        std::lock_guard lock(mutex);
        for (Worker& worker : workers)
        {
            clients.emplace_back(worker.id, worker.client.get());
        }
    }
    const auto startedAt = std::chrono::steady_clock::now();
    VoiceOperationResult aggregate{
        true, "Qwen3-TTS voice workers are ready.", {}, 0.0};
    std::vector<std::future<VoiceOperationResult>> preparations;
    preparations.reserve(clients.size());
    for (const auto& [id, client] : clients)
    {
        preparations.emplace_back(std::async(std::launch::async,
            [client, &preset]() { return client->PrepareVoice(preset); }));
    }
    for (std::size_t index = 0; index < clients.size(); ++index)
    {
        VoiceOperationResult result = preparations[index].get();
        const std::string& id = clients[index].first;
        result.workerId = id;
        if (!result.succeeded)
        {
            return result;
        }
        aggregate.device = result.device;
        aggregate.deviceName = result.deviceName;
        aggregate.dtype = result.dtype;
        aggregate.workerId = id;
        // Reported for the pool, not for the last worker to answer. Workers sit on
        // different cards and can install the low-latency path differently -- an
        // architecture that cannot capture a graph is exactly the case worth hearing
        // about -- so the aggregate takes the weakest result. A pool that says the
        // graph path is on when one of its two cards is on stock generation would send
        // the next session looking for the wrong explanation.
        if (index == 0)
        {
            aggregate.backend = result.backend;
            aggregate.lowLatencyInstalled = result.lowLatencyInstalled;
            aggregate.cudaGraph = result.cudaGraph;
            aggregate.talkerGraph = result.talkerGraph;
            aggregate.backendDetail = result.backendDetail;
        }
        else
        {
            if (!result.lowLatencyInstalled)
            {
                aggregate.lowLatencyInstalled = false;
                aggregate.backend = result.backend;
            }
            aggregate.cudaGraph = aggregate.cudaGraph && result.cudaGraph;
            aggregate.talkerGraph = aggregate.talkerGraph && result.talkerGraph;
            if (!result.backendDetail.empty() &&
                result.backendDetail != aggregate.backendDetail)
            {
                aggregate.backendDetail += "; " + id + ": " + result.backendDetail;
            }
        }
    }
    aggregate.elapsedMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startedAt).count();
    return aggregate;
}

VoiceOperationResult QwenTtsPool::DesignVoice(
    const std::string& text,
    const std::string& description,
    const std::string& language,
    const std::string& outputPath)
{
    std::lock_guard designLock(designMutex);
    if (designClient == nullptr)
        return {false, "No Qwen3-TTS worker is configured.", {}, -1.0};
    VoiceOperationResult result = designClient->DesignVoice(
        text, description, language, outputPath);
    result.workerId = "voice-design-worker";
    // VoiceDesign is deliberately isolated and on-demand. Releasing it cannot evict
    // either persistent conversational clone worker.
    designClient->Shutdown();
    designClient = std::make_unique<QwenTtsClient>();
    designClient->Configure(designSettings);
    return result;
}

VoiceOperationResult QwenTtsPool::Synthesize(
    const std::string& text,
    const VoicePreset& preset,
    const std::string& outputPath,
    const bool latencyCritical)
{
    double poolWaitMilliseconds = 0.0;
    const std::size_t index =
        AcquireWorker(text.size(), latencyCritical, poolWaitMilliseconds);
    QwenTtsClient* client = nullptr;
    std::string id;
    bool rejected = false;
    {
        std::lock_guard lock(mutex);
        if (index >= workers.size() || shuttingDown)
        {
            if (index < workers.size()) workers[index].busy = false;
            rejected = true;
        }
        else
        {
            client = workers[index].client.get();
            id = workers[index].id;
        }
    }
    if (rejected)
    {
        condition.notify_all();
        VoiceOperationResult shuttingDownResult{
            false, "Qwen3-TTS worker pool is shutting down.", {}, -1.0};
        shuttingDownResult.workerPoolWaitMilliseconds = poolWaitMilliseconds;
        return shuttingDownResult;
    }
    const auto startedAt = std::chrono::steady_clock::now();
    VoiceOperationResult result = client->Synthesize(text, preset, outputPath);
    const double wallMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startedAt).count();
    result.workerId = id;
    result.workerPoolWaitMilliseconds = poolWaitMilliseconds;
    ReleaseWorker(index, text.size(), wallMilliseconds);
    return result;
}

VoiceOperationResult QwenTtsPool::SynthesizePcm(
    const std::string& text,
    const VoicePreset& preset,
    const bool latencyCritical)
{
    double poolWaitMilliseconds = 0.0;
    const std::size_t index =
        AcquireWorker(text.size(), latencyCritical, poolWaitMilliseconds);
    QwenTtsClient* client = nullptr;
    std::string id;
    {
        std::lock_guard lock(mutex);
        if (index < workers.size() && !shuttingDown)
        {
            client = workers[index].client.get();
            id = workers[index].id;
        }
        else if (index < workers.size())
        {
            workers[index].busy = false;
        }
    }
    if (client == nullptr)
    {
        condition.notify_all();
        VoiceOperationResult shuttingDownResult{
            false, "Qwen3-TTS worker pool is shutting down.", {}, -1.0};
        shuttingDownResult.workerPoolWaitMilliseconds = poolWaitMilliseconds;
        return shuttingDownResult;
    }
    const auto startedAt = std::chrono::steady_clock::now();
    VoiceOperationResult result = client->SynthesizePcm(text, preset);
    const double wallMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startedAt).count();
    result.workerId = id;
    result.workerPoolWaitMilliseconds = poolWaitMilliseconds;
    ReleaseWorker(index, text.size(), wallMilliseconds);
    return result;
}

std::vector<VoiceOperationResult> QwenTtsPool::SynthesizePcmBatch(
    const std::vector<std::string>& texts,
    const VoicePreset& preset)
{
    std::size_t characters = 0;
    for (const std::string& text : texts) characters += text.size();

    // Not latency-critical: the phrase the listener is waiting on already went out on
    // its own, and this call is the work queued behind it.
    double poolWaitMilliseconds = 0.0;
    const std::size_t index = AcquireWorker(characters, false, poolWaitMilliseconds);
    QwenTtsClient* client = nullptr;
    std::string id;
    {
        std::lock_guard lock(mutex);
        if (index < workers.size() && !shuttingDown)
        {
            client = workers[index].client.get();
            id = workers[index].id;
        }
        else if (index < workers.size())
        {
            workers[index].busy = false;
        }
    }
    if (client == nullptr)
    {
        condition.notify_all();
        VoiceOperationResult shuttingDownResult{
            false, "Qwen3-TTS worker pool is shutting down.", {}, -1.0};
        shuttingDownResult.workerPoolWaitMilliseconds = poolWaitMilliseconds;
        return {shuttingDownResult};
    }

    const auto startedAt = std::chrono::steady_clock::now();
    std::vector<VoiceOperationResult> results =
        client->SynthesizePcmBatch(texts, preset);
    const double wallMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startedAt).count();
    for (VoiceOperationResult& result : results)
    {
        result.workerId = id;
        // The whole batch waited once, together. Attributing that wait to each clip is
        // what the number means: every phrase in it started speaking that much later.
        result.workerPoolWaitMilliseconds = poolWaitMilliseconds;
    }
    ReleaseWorker(index, characters, wallMilliseconds);
    return results;
}

void QwenTtsPool::CancelActiveRequests()
{
    std::vector<QwenTtsClient*> clients;
    {
        std::lock_guard lock(mutex);
        for (Worker& worker : workers) clients.push_back(worker.client.get());
    }
    for (QwenTtsClient* client : clients) client->CancelActiveRequest();
    std::lock_guard designLock(designMutex);
    if (designClient) designClient->CancelActiveRequest();
}

void QwenTtsPool::RequestShutdown()
{
    {
        std::lock_guard lock(mutex);
        shuttingDown = true;
    }
    condition.notify_all();
    CancelActiveRequests();
}

void QwenTtsPool::Shutdown()
{
    RequestShutdown();
    std::vector<std::unique_ptr<QwenTtsClient>> clients;
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [this]
        {
            return std::none_of(workers.begin(), workers.end(),
                [](const Worker& worker) { return worker.busy; });
        });
        for (Worker& worker : workers) clients.push_back(std::move(worker.client));
        workers.clear();
    }
    condition.notify_all();
    for (auto& client : clients) client->Shutdown();
    std::lock_guard designLock(designMutex);
    if (designClient) designClient->Shutdown();
    designClient.reset();
}

std::size_t SelectIdleVoiceWorker(
    const std::vector<VoiceWorkerState>& workers,
    const std::size_t characters,
    const bool latencyCritical)
{
    // A preference, not a pin. Worker 0 is where the resource planner put the phrase
    // the listener is waiting on, so it wins whenever it is free.
    //
    // It does not win while it is busy. The planner puts the newer chat GPU first on
    // the assumption that newer is faster, and the live session contradicted that
    // under load: the RTX 5070 that also carries chat averaged a real-time factor of
    // 5.78 against the RTX 2070's 4.13, and the very first phrase of the session ran
    // at 6.44 there. Waiting for a busy card to become the "right" one is how a first
    // phrase ends up slower than it would have been anywhere else.
    if (latencyCritical && !workers.empty() && !workers.front().busy)
    {
        return 0;
    }

    std::size_t best = workers.size();
    double bestMilliseconds = 0.0;
    for (std::size_t index = 0; index < workers.size(); ++index)
    {
        const VoiceWorkerState& worker = workers[index];
        if (worker.busy) continue;
        // Every candidate is idle, so predicted duration and predicted finish are the
        // same ordering and there is no need to reason about clocks here.
        const double predictedMilliseconds = worker.fixedOverheadMilliseconds +
            worker.millisecondsPerCharacter *
                static_cast<double>(std::max<std::size_t>(1, characters));
        if (best == workers.size() || predictedMilliseconds < bestMilliseconds)
        {
            bestMilliseconds = predictedMilliseconds;
            best = index;
        }
    }
    return best;
}

std::size_t QwenTtsPool::AcquireWorker(
    const std::size_t characters,
    const bool latencyCritical,
    double& outWaitMilliseconds)
{
    const auto waitStarted = std::chrono::steady_clock::now();
    const auto waited = [&waitStarted]
    {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - waitStarted).count();
    };

    std::unique_lock lock(mutex);
    std::vector<VoiceWorkerState> states;
    while (!shuttingDown)
    {
        // Rebuilt on every pass, never carried across the wait. A choice made before
        // blocking describes a pool that has since changed -- the release that woke
        // this caller is exactly the event that invalidates it.
        states.clear();
        states.reserve(workers.size());
        for (const Worker& worker : workers)
        {
            states.push_back({worker.busy, worker.fixedOverheadMilliseconds,
                worker.millisecondsPerCharacter});
        }
        const std::size_t best =
            SelectIdleVoiceWorker(states, characters, latencyCritical);
        if (best < workers.size())
        {
            workers[best].busy = true;
            outWaitMilliseconds = waited();
            return best;
        }
        condition.wait(lock);
    }
    outWaitMilliseconds = waited();
    return workers.size();
}

void QwenTtsPool::ReleaseWorker(
    const std::size_t index,
    const std::size_t characters,
    const double milliseconds)
{
    {
        std::lock_guard lock(mutex);
        if (index < workers.size())
        {
            Worker& worker = workers[index];
            const double sample = milliseconds /
                static_cast<double>(std::max<std::size_t>(1, characters));
            worker.millisecondsPerCharacter = worker.completed == 0
                ? sample
                : worker.millisecondsPerCharacter * 0.72 + sample * 0.28;
            ++worker.completed;
            if (worker.firstPhraseMilliseconds < 0.0)
            {
                worker.firstPhraseMilliseconds = milliseconds;
            }
            else
            {
                worker.firstPhraseMilliseconds =
                    worker.firstPhraseMilliseconds * 0.8 + milliseconds * 0.2;
            }
            const double inferredFixed = std::max(
                0.0, milliseconds - worker.millisecondsPerCharacter *
                    static_cast<double>(characters));
            worker.fixedOverheadMilliseconds = worker.completed == 1
                ? inferredFixed
                : worker.fixedOverheadMilliseconds * 0.8 + inferredFixed * 0.2;
            worker.busy = false;
        }
    }
    condition.notify_one();
}

} // namespace revia::speech
