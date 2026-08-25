#include "Speech/qwenTtsPool.h"

#include <algorithm>
#include <chrono>
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
        worker.millisecondsPerCharacter = 35.0 + workers.size() * 8.0;
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

VoiceOperationResult QwenTtsPool::PrepareCloneModel()
{
    std::vector<std::pair<std::string, QwenTtsClient*>> clients;
    {
        std::lock_guard lock(mutex);
        for (Worker& worker : workers)
        {
            clients.emplace_back(worker.id, worker.client.get());
        }
    }
    VoiceOperationResult aggregate{
        true, "Qwen3-TTS voice workers are ready.", {}, 0.0};
    for (const auto& [id, client] : clients)
    {
        VoiceOperationResult result = client->PrepareCloneModel();
        result.workerId = id;
        if (!result.succeeded)
        {
            return result;
        }
        aggregate.elapsedMilliseconds += std::max(0.0, result.elapsedMilliseconds);
        aggregate.device = result.device;
        aggregate.deviceName = result.deviceName;
        aggregate.dtype = result.dtype;
        aggregate.workerId = id;
    }
    return aggregate;
}

VoiceOperationResult QwenTtsPool::DesignVoice(
    const std::string& text,
    const std::string& description,
    const std::string& language,
    const std::string& outputPath)
{
    QwenTtsClient* client = nullptr;
    std::string id;
    {
        std::lock_guard lock(mutex);
        if (!workers.empty())
        {
            client = workers.front().client.get();
            id = workers.front().id;
        }
    }
    if (client == nullptr)
        return {false, "No Qwen3-TTS worker is configured.", {}, -1.0};
    VoiceOperationResult result = client->DesignVoice(
        text, description, language, outputPath);
    result.workerId = id;
    return result;
}

VoiceOperationResult QwenTtsPool::Synthesize(
    const std::string& text,
    const VoicePreset& preset,
    const std::string& outputPath)
{
    const std::size_t index = AcquireWorker(text.size());
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
        return {false, "Qwen3-TTS worker pool is shutting down.", {}, -1.0};
    }
    const auto startedAt = std::chrono::steady_clock::now();
    VoiceOperationResult result = client->Synthesize(text, preset, outputPath);
    const double wallMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startedAt).count();
    result.workerId = id;
    ReleaseWorker(index, text.size(), wallMilliseconds);
    return result;
}

void QwenTtsPool::CancelActiveRequests()
{
    std::vector<QwenTtsClient*> clients;
    {
        std::lock_guard lock(mutex);
        for (Worker& worker : workers) clients.push_back(worker.client.get());
    }
    for (QwenTtsClient* client : clients) client->CancelActiveRequest();
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
}

std::size_t QwenTtsPool::AcquireWorker(const std::size_t characters)
{
    std::unique_lock lock(mutex);
    condition.wait(lock, [this]
    {
        return shuttingDown || std::any_of(workers.begin(), workers.end(),
            [](const Worker& worker) { return !worker.busy; });
    });
    if (shuttingDown) return workers.size();
    std::size_t best = workers.size();
    double bestPrediction = std::numeric_limits<double>::max();
    for (std::size_t index = 0; index < workers.size(); ++index)
    {
        if (workers[index].busy) continue;
        const double prediction = workers[index].millisecondsPerCharacter *
            static_cast<double>(std::max<std::size_t>(1, characters));
        if (prediction < bestPrediction)
        {
            bestPrediction = prediction;
            best = index;
        }
    }
    if (best < workers.size()) workers[best].busy = true;
    return best;
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
            worker.busy = false;
        }
    }
    condition.notify_one();
}

} // namespace revia::speech
