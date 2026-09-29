#include "Speech/speakerEmbeddingClient.h"

#include "Core/localApiKey.h"
#include "Core/runtimePath.h"

#include <chrono>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <thread>

namespace revia::speech
{

namespace
{
using json = nlohmann::json;
}

SpeakerEmbeddingClient::~SpeakerEmbeddingClient()
{
    Shutdown();
}

void SpeakerEmbeddingClient::Configure(const speechRecognitionSettings& settings)
{
    std::lock_guard lock(mutex);
    configuration = settings;
    host = "127.0.0.1";
    port = settings.speakerPort;
    external = false;
    closed = false;
    if (token.empty()) token = core::GenerateLocalApiKey();
}

void SpeakerEmbeddingClient::UseEndpoint(std::string inputHost, const int inputPort, std::string inputToken)
{
    std::lock_guard lock(mutex);
    host = std::move(inputHost);
    port = inputPort;
    token = std::move(inputToken);
    external = true;
    closed = false;
    configuration.bSpeakerIdentificationEnabled = true;
}

bool SpeakerEmbeddingClient::Enabled() const
{
    return configuration.bSpeakerIdentificationEnabled;
}

bool SpeakerEmbeddingClient::EnsureAvailable(std::string& outError)
{
    if (closed)
    {
        outError = "The speaker worker is shutting down.";
        return false;
    }
    if (!configuration.bSpeakerIdentificationEnabled)
    {
        outError = "Speaker identification is off.";
        return false;
    }
    const auto healthy = [this]
    {
        httplib::Client client(host, port);
        client.set_connection_timeout(1);
        client.set_read_timeout(2);
        const auto response = client.Get("/health", httplib::Headers{{"Authorization", "Bearer " + token}});
        return response && response->status == 200;
    };
    if (healthy()) return true;
    if (external)
    {
        outError = "The speaker worker at " + host + ":" + std::to_string(port) + " did not answer.";
        return false;
    }
    if (!process.IsRunning())
    {
        core::PythonWorkerLaunch launch;
        launch.script = configuration.speakerServiceScript;
        launch.pythonExecutable = configuration.speakerPythonExecutable;
        launch.port = port;
        launch.logName = "speaker-id";
        launch.displayName = "speaker worker";
        launch.arguments = {"--host", host, "--port", std::to_string(port), "--token", token,
            "--model", core::ResolveRuntimePath(std::filesystem::path(configuration.speakerModelPath)).string()};
        if (!process.Start(launch, outError)) return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (closed || !process.IsRunning())
        {
            outError = "The speaker worker exited during startup. Check Logs/speaker-id-" +
                std::to_string(port) + ".stderr.log.";
            return false;
        }
        if (healthy()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    outError = "Timed out waiting for the speaker worker.";
    return false;
}

bool SpeakerEmbeddingClient::IsAvailable(std::string& outDetail)
{
    std::lock_guard lock(mutex);
    return EnsureAvailable(outDetail);
}

std::optional<std::vector<float>> SpeakerEmbeddingClient::Embed(
    const std::filesystem::path& wavePath, std::string& outError)
{
    std::lock_guard lock(mutex);
    if (!EnsureAvailable(outError)) return std::nullopt;
    httplib::Client client(host, port);
    client.set_connection_timeout(2);
    client.set_read_timeout(20);
    const json body = {{"wav_path", std::filesystem::absolute(wavePath).string()}};
    const auto response = client.Post("/v1/speaker/embed",
        httplib::Headers{{"Authorization", "Bearer " + token}}, body.dump(), "application/json");
    if (!response)
    {
        outError = "The speaker worker did not answer (" + httplib::to_string(response.error()) + ").";
        return std::nullopt;
    }
    try
    {
        const json parsed = json::parse(response->body);
        if (response->status != 200 || !parsed.value("succeeded", false))
        {
            outError = parsed.value("message", "HTTP " + std::to_string(response->status) + ".");
            return std::nullopt;
        }
        std::vector<float> embedding = parsed.at("embedding").get<std::vector<float>>();
        if (embedding.size() < 8)
        {
            outError = "The embedding was too short to use.";
            return std::nullopt;
        }
        return embedding;
    }
    catch (const std::exception& error)
    {
        outError = std::string("The speaker worker's answer could not be read: ") + error.what();
        return std::nullopt;
    }
}

void SpeakerEmbeddingClient::Shutdown()
{
    std::lock_guard lock(mutex);
    closed = true;
    process.Stop();
}

} // namespace revia::speech
