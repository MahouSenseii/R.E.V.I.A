#include "Speech/qwenTtsClient.h"

#include "Core/localApiKey.h"

#include <chrono>
#include <filesystem>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <thread>

namespace revia::speech
{

QwenTtsClient::~QwenTtsClient()
{
    Shutdown();
}

void QwenTtsClient::Configure(speechSettings settings)
{
    std::lock_guard lock(mutex);
    shuttingDown.store(false);
    configuration = std::move(settings);
    if (apiKey.empty())
    {
        apiKey = revia::core::GenerateLocalApiKey();
    }
}

bool QwenTtsClient::IsAvailable(std::string& outDetail)
{
    std::lock_guard lock(mutex);
    return EnsureAvailable(outDetail);
}

VoiceOperationResult QwenTtsClient::PrepareCloneModel()
{
    std::lock_guard lock(mutex);
    std::string error;
    if (!EnsureAvailable(error))
    {
        return {false, std::move(error), {}, -1.0};
    }
    return Post("/prepare", R"({"model":"clone"})");
}

VoiceOperationResult QwenTtsClient::DesignVoice(
    const std::string& text,
    const std::string& description,
    const std::string& language,
    const std::string& outputPath)
{
    std::lock_guard lock(mutex);
    std::string error;
    if (!EnsureAvailable(error))
    {
        return {false, std::move(error), {}, -1.0};
    }
    const nlohmann::json request = {
        {"text", text},
        {"description", description},
        {"language", language},
        {"output_path", outputPath}
    };
    return Post("/v1/voice-design", request.dump());
}

VoiceOperationResult QwenTtsClient::Synthesize(
    const std::string& text,
    const VoicePreset& preset,
    const std::string& outputPath)
{
    std::lock_guard lock(mutex);
    std::string error;
    if (!EnsureAvailable(error))
    {
        return {false, std::move(error), {}, -1.0};
    }
    const nlohmann::json request = {
        {"text", text},
        {"language", preset.language},
        {"reference_audio", std::filesystem::absolute(preset.referenceAudioPath).string()},
        {"reference_text", preset.referenceText},
        {"output_path", outputPath}
    };
    return Post("/v1/audio/speech", request.dump());
}

void QwenTtsClient::Shutdown()
{
    shuttingDown.store(true);
    std::lock_guard processLock(processMutex);
    process.Stop();
}

void QwenTtsClient::CancelActiveRequest()
{
    shuttingDown.store(true);
    {
        std::lock_guard processLock(processMutex);
        process.Stop();
    }
    shuttingDown.store(false);
}

bool QwenTtsClient::EnsureAvailable(std::string& outError)
{
    if (shuttingDown.load())
    {
        outError = "Qwen3-TTS is shutting down.";
        return false;
    }
    const auto healthy = [this, &outError]()
    {
        httplib::Client client(configuration.qwenHost, configuration.qwenPort);
        client.set_connection_timeout(1);
        client.set_read_timeout(2);
        httplib::Headers headers{{"Authorization", "Bearer " + apiKey}};
        const auto response = client.Get("/health", headers);
        if (response && response->status == 200)
        {
            try
            {
                const auto body = nlohmann::json::parse(response->body);
                outError = body.value("detail", "Qwen3-TTS is ready.");
            }
            catch (...)
            {
                outError = "Qwen3-TTS is ready.";
            }
            return true;
        }
        return false;
    };
    if (healthy())
    {
        return true;
    }
    {
        std::lock_guard processLock(processMutex);
        if (!process.IsRunning() && !process.Start(configuration, apiKey, outError))
        {
            return false;
        }
    }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(configuration.qwenStartupTimeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        bool running = false;
        {
            std::lock_guard processLock(processMutex);
            running = process.IsRunning();
        }
        if (!running || shuttingDown.load())
        {
            outError = "The Qwen3-TTS worker exited during startup. Check Logs/qwen-tts.stderr.log.";
            return false;
        }
        if (healthy())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    outError = "Timed out waiting for the Qwen3-TTS worker.";
    return false;
}

VoiceOperationResult QwenTtsClient::Post(
    const std::string& endpoint,
    const std::string& body)
{
    httplib::Client client(configuration.qwenHost, configuration.qwenPort);
    client.set_connection_timeout(5);
    client.set_read_timeout(configuration.qwenRequestTimeoutSeconds);
    client.set_write_timeout(10);
    httplib::Headers headers{{"Authorization", "Bearer " + apiKey}};
    const auto response = client.Post(endpoint, headers, body, "application/json");
    if (!response)
    {
        return {false, "The Qwen3-TTS worker did not return a response.", {}, -1.0};
    }
    try
    {
        const auto data = nlohmann::json::parse(response->body);
        VoiceOperationResult result;
        result.succeeded = response->status == 200 && data.value("succeeded", false);
        result.message = data.value("message", result.succeeded
            ? "Qwen3-TTS completed."
            : "Qwen3-TTS failed.");
        result.outputPath = data.value("output_path", "");
        result.elapsedMilliseconds = data.value("elapsed_ms", -1.0);
        result.device = data.value("device", "");
        result.deviceName = data.value("device_name", "");
        result.dtype = data.value("dtype", "");
        return result;
    }
    catch (const std::exception& exception)
    {
        return {false, std::string("Qwen3-TTS returned invalid JSON: ") + exception.what(), {}, -1.0};
    }
}

} // namespace revia::speech
