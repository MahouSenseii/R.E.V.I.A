#include "Memory/memoryNliWorker.h"

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
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace revia::memory
{
namespace
{
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
constexpr std::size_t MaximumResponseBytes = 8192;

Json ParseProtocolJson(const std::string& text)
{
    std::vector<std::set<std::string>> keys;
    return Json::parse(text, [&](int depth, Json::parse_event_t event, Json& value)
    {
        if (depth > 4)
            throw std::runtime_error("NLI protocol nesting exceeds its fixed shape.");
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::runtime_error("Duplicate NLI protocol key.");
        else if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        return true;
    });
}

bool ValidText(const std::string& text, const std::size_t maximum)
{
    return !text.empty() && text.size() <= maximum && text.find('\0') == std::string::npos && utf8::IsValid(text);
}
}

struct MemoryNliWorker::Impl
{
    std::mutex operation;
    mutable std::mutex state;
    std::unique_ptr<process::OwnedProcess> child = std::make_unique<process::OwnedProcess>();
    std::stop_source lifetime;
    std::atomic<bool> available = false;
    std::string error;
    std::string secret;
    int port = 0;
    int timeoutMs = 2000;

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

MemoryNliWorker::MemoryNliWorker() : impl(std::make_unique<Impl>()) {}
MemoryNliWorker::~MemoryNliWorker() { Stop(); }

bool MemoryNliWorker::Start(const MemoryNliSettings& settings, const std::stop_token stopToken)
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
        return fail("Semantic duplicate scoring is disabled.");
    std::error_code filesystemError;
    if (!settings.pythonExecutable.is_absolute() || !settings.workerScript.is_absolute() ||
        !settings.modelDirectory.is_absolute() ||
        !std::filesystem::is_regular_file(settings.pythonExecutable, filesystemError) ||
        !std::filesystem::is_regular_file(settings.workerScript, filesystemError) ||
        !std::filesystem::is_directory(settings.modelDirectory, filesystemError))
        return fail("The local CPU memory entailment worker is not installed.");
    if (settings.cpuThreads < 1 || settings.cpuThreads > 8 || settings.timeoutMs < 1 || settings.timeoutMs > 10000 ||
        settings.startupTimeoutMs < 1 || settings.startupTimeoutMs > 120000)
        return fail("Memory entailment worker settings are outside supported bounds.");
    if (stopToken.stop_requested() || ownerToken.stop_requested())
        return fail("Memory entailment worker startup was cancelled.");
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
        return fail("Could not start the owned CPU memory entailment worker.");
    const auto deadline = Clock::now() + std::chrono::milliseconds(settings.startupTimeoutMs);
    std::string receipt;
    while (Clock::now() < deadline && !stopToken.stop_requested() && !ownerToken.stop_requested())
    {
        receipt += impl->child->Read(std::chrono::milliseconds(20));
        if (receipt.size() > MaximumResponseBytes)
            return fail("The memory entailment worker returned an invalid startup receipt.");
        if (receipt.find('\n') != std::string::npos)
            break;
        if (!impl->child->Running())
            return fail("The local CPU memory entailment worker failed to load.");
    }
    if (stopToken.stop_requested() || ownerToken.stop_requested())
        return fail("Memory entailment worker startup was cancelled.");
    if (Clock::now() >= deadline)
        return fail("Memory entailment worker startup timed out.");
    try
    {
        const auto body = ParseProtocolJson(receipt);
        const auto& port = body.at("port");
        const auto token = body.at("token").get<std::string>();
        if (body.at("ready") != true || body.at("device") != "cpu" ||
            body.at("model") != MemoryNliModelIdentity || !port.is_number_integer() || port < 1 || port > 65535 ||
            token.size() != 64 || !std::all_of(token.begin(), token.end(), [](const char value) {
                return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
            }))
            return fail("The memory entailment worker returned an invalid startup receipt.");
        impl->port = port.get<int>();
        impl->secret = token;
        impl->available.store(true);
        return true;
    }
    catch (...)
    {
        return fail("The memory entailment worker returned an invalid startup receipt.");
    }
}

void MemoryNliWorker::Stop()
{
    {
        std::lock_guard lock(impl->state);
        impl->lifetime.request_stop();
    }
    std::lock_guard operation(impl->operation);
    impl->StopChild();
}

bool MemoryNliWorker::IsAvailable() const { return impl->available.load(); }

std::string MemoryNliWorker::LastError() const
{
    std::lock_guard lock(impl->state);
    return impl->error;
}

NliPairResult MemoryNliWorker::Compare(const std::string& left, const std::string& right,
                                     const std::stop_token stopToken)
{
    NliPairResult output;
    const auto started = Clock::now();
    if (stopToken.stop_requested() || !ValidText(left, 1024) || !ValidText(right, 1024))
        return output;
    std::unique_lock operation(impl->operation, std::try_to_lock);
    if (!operation.owns_lock() || !impl->available.load())
        return output;
    if (!impl->child->Running())
    {
        impl->StopChild();
        return output;
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
            requestStop.request_stop();
    });
    std::string body;
    httplib::Request request;
    request.method = "POST";
    request.path = "/compare";
    request.headers = {{"Authorization", "Bearer " + impl->secret}, {"Content-Type", "application/json"}};
    request.body = Json{{"left", left}, {"right", right}}.dump();
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
    if (Clock::now() >= started + timeout)
        requestStop.request_stop();
    deadline.request_stop();
    deadline.join();
    if (requestStop.stop_requested())
    {
        impl->StopChild();
        return output;
    }
    if (!received || response.status != 200)
        return output;
    try
    {
        const auto decoded = ParseProtocolJson(body);
        if (!decoded.is_object() || decoded.size() != 4 || decoded.at("model") != MemoryNliModelIdentity ||
            !decoded.at("truncated").is_boolean() || decoded.at("truncated").get<bool>())
            return output;
        const auto read = [](const Json& row, double& entailment, double& contradiction)
        {
            if (!row.is_object() || row.size() != 2 || !row.at("entailment").is_number() ||
                !row.at("contradiction").is_number())
                return false;
            entailment = row.at("entailment").get<double>();
            contradiction = row.at("contradiction").get<double>();
            return std::isfinite(entailment) && std::isfinite(contradiction) &&
                entailment >= 0 && entailment <= 1 && contradiction >= 0 && contradiction <= 1 &&
                entailment + contradiction <= 1.0001;
        };
        NliPairResult validated;
        if (!read(decoded.at("forward"), validated.forwardEntailment, validated.forwardContradiction) ||
            !read(decoded.at("reverse"), validated.reverseEntailment, validated.reverseContradiction))
            return output;
        validated.modelIdentity = MemoryNliModelIdentity;
        validated.succeeded = true;
        return validated;
    }
    catch (...)
    {
        return output;
    }
}
}
