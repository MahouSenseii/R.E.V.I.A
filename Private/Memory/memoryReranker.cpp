#include "Memory/memoryReranker.h"

#include "Actions/actionTypes.h"
#include "Core/utf8.h"
#include "Process/ownedProcess.h"
#include "../LLM/LLamaCPP/cancellableHttpClient.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <nlohmann/json.hpp>
#include <numeric>
#include <thread>

namespace revia::memory
{
namespace
{
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
constexpr std::size_t MaximumCandidates = 30;
constexpr std::size_t MaximumResponseBytes = 8192;

bool ValidScores(const std::vector<RerankScore>& scores, const std::size_t count)
{
    if (count > MaximumCandidates || scores.size() != count)
        return false;
    std::vector<bool> seen(count, false);
    for (const auto& score : scores)
    {
        if (score.index >= count || seen[score.index] || !std::isfinite(score.score))
            return false;
        seen[score.index] = true;
    }
    return true;
}

bool ValidText(const std::string& text, const std::size_t maximum)
{
    return !text.empty() && text.size() <= maximum && text.find('\0') == std::string::npos && utf8::IsValid(text);
}
}

std::vector<std::size_t> RerankOrder(const RerankResult& result, const std::size_t documentCount)
{
    std::vector<std::size_t> order(documentCount);
    std::iota(order.begin(), order.end(), 0);
    if (!result.succeeded || result.status != RerankStatus::Succeeded || !ValidScores(result.scores, documentCount))
        return order;
    std::vector<double> scores(documentCount);
    for (const auto& score : result.scores)
        scores[score.index] = score.score;
    std::stable_sort(order.begin(), order.end(), [&](const auto left, const auto right) { return scores[left] > scores[right]; });
    return order;
}

struct MemoryReranker::Impl
{
    std::mutex operation;
    mutable std::mutex state;
    std::unique_ptr<process::OwnedProcess> child = std::make_unique<process::OwnedProcess>();
    std::stop_source lifetime;
    std::atomic<bool> available = false;
    std::string error;
    std::string secret;
    int port = 0;
    int timeoutMs = 1000;

    void SetError(std::string value)
    {
        std::lock_guard lock(state);
        error = std::move(value);
    }

    void StopChild()
    {
        available.store(false);
        child->Terminate();
        // OwnedProcess is a single-start owner; a restart gets fresh pipe state.
        child = std::make_unique<process::OwnedProcess>();
        secret.clear();
        port = 0;
    }
};

MemoryReranker::MemoryReranker() : impl(std::make_unique<Impl>()) {}
MemoryReranker::~MemoryReranker() { Stop(); }

bool MemoryReranker::Start(const RerankerSettings& settings, const std::stop_token stopToken)
{
    Stop();
    std::lock_guard operation(impl->operation);
    std::stop_token ownerToken;
    {
        std::lock_guard lock(impl->state);
        impl->lifetime = std::stop_source();
        ownerToken = impl->lifetime.get_token();
        impl->error.clear();
    }
    const auto fail = [&](const std::string& message)
    {
        impl->SetError(message);
        impl->StopChild();
        return false;
    };
    if (!settings.enabled)
        return fail("Memory reranking is disabled.");
    std::error_code filesystemError;
    if (!settings.pythonExecutable.is_absolute() || !settings.workerScript.is_absolute() ||
        !settings.modelDirectory.is_absolute() ||
        !std::filesystem::is_regular_file(settings.pythonExecutable, filesystemError) ||
        !std::filesystem::is_regular_file(settings.workerScript, filesystemError) ||
        !std::filesystem::is_directory(settings.modelDirectory, filesystemError))
        return fail("The local CPU memory reranker is not installed.");
    if (settings.cpuThreads < 1 || settings.cpuThreads > 16 || settings.timeoutMs < 1 || settings.timeoutMs > 10000 ||
        settings.startupTimeoutMs < 1 || settings.startupTimeoutMs > 120000)
        return fail("Memory reranker settings are outside supported bounds.");
    if (stopToken.stop_requested() || ownerToken.stop_requested())
        return fail("Memory reranker startup was cancelled.");
    impl->timeoutMs = settings.timeoutMs;
    process::OwnedProcessRequest request;
    request.executable = settings.pythonExecutable;
    request.workingDirectory = settings.workerScript.parent_path();
    request.arguments = {"-I", "-u", actions::PathToUtf8(settings.workerScript), "--model-directory",
                         actions::PathToUtf8(settings.modelDirectory), "--threads", std::to_string(settings.cpuThreads)};
    request.environment = {{"HF_HUB_OFFLINE", "1"}, {"TRANSFORMERS_OFFLINE", "1"}, {"HF_HUB_DISABLE_TELEMETRY", "1"},
                           {"CUDA_VISIBLE_DEVICES", ""}, {"TOKENIZERS_PARALLELISM", "false"}};
    request.maxPendingOutputBytes = MaximumResponseBytes;
    request.maxErrorBytes = 1024;
    std::string error;
    if (!impl->child->Start(request, error))
        return fail("Could not start the owned CPU memory reranker.");
    const auto deadline = Clock::now() + std::chrono::milliseconds(settings.startupTimeoutMs);
    std::string receipt;
    while (Clock::now() < deadline && !stopToken.stop_requested() && !ownerToken.stop_requested())
    {
        receipt += impl->child->Read(std::chrono::milliseconds(20));
        if (receipt.size() > MaximumResponseBytes)
            return fail("The memory reranker returned an invalid startup receipt.");
        if (receipt.find('\n') != std::string::npos)
            break;
        if (!impl->child->Running())
            return fail("The local CPU memory reranker failed to load.");
    }
    if (stopToken.stop_requested() || ownerToken.stop_requested())
        return fail("Memory reranker startup was cancelled.");
    if (Clock::now() >= deadline)
        return fail("Memory reranker startup timed out.");
    try
    {
        const auto body = Json::parse(receipt);
        const auto& port = body.at("port");
        const auto token = body.at("token").get<std::string>();
        if (body.at("ready") != true || body.at("device") != "cpu" ||
            body.at("model") != "cross-encoder/ms-marco-MiniLM-L6-v2" || !port.is_number_integer() || port < 1 || port > 65535 ||
            token.size() != 64 || !std::all_of(token.begin(), token.end(), [](const char value) {
                return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
            }))
            return fail("The memory reranker returned an invalid startup receipt.");
        impl->port = port.get<int>();
        impl->secret = token;
        impl->available.store(true);
        return true;
    }
    catch (...)
    {
        return fail("The memory reranker returned an invalid startup receipt.");
    }
}

void MemoryReranker::Stop()
{
    {
        std::lock_guard lock(impl->state);
        impl->lifetime.request_stop();
    }
    std::lock_guard operation(impl->operation);
    impl->StopChild();
}

bool MemoryReranker::IsAvailable() const { return impl->available.load(); }

std::string MemoryReranker::LastError() const
{
    std::lock_guard lock(impl->state);
    return impl->error;
}

RerankResult MemoryReranker::Rerank(const std::string& query, const std::vector<std::string>& documents,
                                  const std::stop_token stopToken)
{
    const auto started = Clock::now();
    RerankResult output;
    const auto finish = [&](const RerankStatus status)
    {
        output.status = status;
        output.succeeded = status == RerankStatus::Succeeded;
        if (!output.succeeded)
            output.scores.clear();
        output.elapsedMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        return output;
    };
    if (stopToken.stop_requested())
        return finish(RerankStatus::Cancelled);
    if (!ValidText(query, 4096) || documents.empty() || documents.size() > MaximumCandidates ||
        std::any_of(documents.begin(), documents.end(), [](const auto& text) { return !ValidText(text, 8192); }))
        return finish(RerankStatus::InvalidInput);
    std::unique_lock operation(impl->operation, std::try_to_lock);
    if (!operation.owns_lock())
        return finish(RerankStatus::Busy);
    if (!impl->available.load())
        return finish(RerankStatus::Unavailable);
    if (!impl->child->Running())
    {
        impl->StopChild();
        return finish(RerankStatus::Unavailable);
    }
    std::stop_token ownerToken;
    {
        std::lock_guard lock(impl->state);
        ownerToken = impl->lifetime.get_token();
    }
    std::stop_source requestStop;
    std::stop_callback callerCancellation(stopToken, [&] { requestStop.request_stop(); });
    std::stop_callback ownerCancellation(ownerToken, [&] { requestStop.request_stop(); });
    llm::CancellableHttpClient client("127.0.0.1", impl->port, requestStop.get_token());
    const auto timeout = std::chrono::milliseconds(impl->timeoutMs);
    client.set_connection_timeout(timeout);
    client.set_read_timeout(timeout);
    client.set_write_timeout(timeout);
    client.set_follow_location(false);
    std::stop_callback cancelHttp(requestStop.get_token(), [&] { client.stop(); });
    std::jthread deadline([&](const std::stop_token timerStop)
    {
        std::mutex mutex;
        std::condition_variable_any changed;
        std::unique_lock lock(mutex);
        changed.wait_until(lock, timerStop, started + timeout, [] { return false; });
        if (!timerStop.stop_requested())
        {
            requestStop.request_stop();
        }
    });
    std::string body;
    httplib::Request request;
    request.method = "POST";
    request.path = "/rerank";
    request.headers = {{"Authorization", "Bearer " + impl->secret}, {"Content-Type", "application/json"}};
    request.body = Json{{"query", query}, {"documents", documents}}.dump();
    request.content_receiver = [&](const char* bytes, const std::size_t count, std::uint64_t, std::uint64_t)
    {
        if (body.size() + count > MaximumResponseBytes)
            return false;
        body.append(bytes, count);
        return true;
    };
    httplib::Response response;
    httplib::Error transportError;
    const bool received = !requestStop.stop_requested() && client.send(request, response, transportError);
    const bool deadlineExpired = Clock::now() >= started + timeout;
    if (deadlineExpired)
        requestStop.request_stop();
    deadline.request_stop();
    deadline.join();
    if (requestStop.stop_requested())
    {
        // Caller or owner cancellation (Stop, barge-in, new turn) must not disable
        // reranking for the rest of the session. Owner Stop() still kills the child
        // once it acquires impl->operation; a real deadline expiry still terminates
        // the possibly-hung worker so a lifecycle Start can bring up a fresh one.
        const bool callerCancelled = stopToken.stop_requested() || ownerToken.stop_requested();
        if (callerCancelled && !deadlineExpired)
            return finish(RerankStatus::Cancelled);
        impl->StopChild();
        return finish(callerCancelled ? RerankStatus::Cancelled : RerankStatus::TimedOut);
    }
    if (!received || response.status != 200)
        return finish(RerankStatus::Unavailable);
    try
    {
        const auto decoded = Json::parse(body);
        if (!decoded.is_object() || decoded.size() != 1 || !decoded.at("scores").is_array() ||
            decoded.at("scores").size() != documents.size())
            return finish(RerankStatus::InvalidResponse);
        for (const auto& entry : decoded.at("scores"))
        {
            if (!entry.is_object() || entry.size() != 2 || !entry.at("index").is_number_unsigned() ||
                !entry.at("score").is_number())
                return finish(RerankStatus::InvalidResponse);
            output.scores.push_back({entry.at("index").get<std::size_t>(), entry.at("score").get<double>()});
        }
        if (!ValidScores(output.scores, documents.size()))
            return finish(RerankStatus::InvalidResponse);
        return finish(RerankStatus::Succeeded);
    }
    catch (...)
    {
        return finish(RerankStatus::InvalidResponse);
    }
}
}
