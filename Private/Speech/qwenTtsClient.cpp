#include "Speech/qwenTtsClient.h"

#include "Core/localApiKey.h"

#include <chrono>
#include <filesystem>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <thread>

namespace revia::speech
{

namespace
{

nlohmann::json VoiceRequest(
    const std::string& text,
    const VoicePreset& preset)
{
    return {
        {"text", text},
        {"language", preset.language},
        {"reference_audio", std::filesystem::absolute(
            preset.referenceAudioPath).string()},
        {"reference_text", preset.referenceText}
    };
}

double HeaderDouble(const httplib::Response& response, const char* name)
{
    try
    {
        const std::string value = response.get_header_value(name);
        return value.empty() ? -1.0 : std::stod(value);
    }
    catch (...)
    {
        return -1.0;
    }
}

int HeaderInt(const httplib::Response& response, const char* name)
{
    try
    {
        const std::string value = response.get_header_value(name);
        return value.empty() ? 0 : std::stoi(value);
    }
    catch (...)
    {
        return 0;
    }
}

} // namespace

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

VoiceOperationResult QwenTtsClient::PrepareVoice(const VoicePreset& preset)
{
    std::lock_guard lock(mutex);
    std::string error;
    if (!EnsureAvailable(error))
    {
        return {false, std::move(error), {}, -1.0};
    }
    nlohmann::json request = VoiceRequest("prepare", preset);
    request.erase("text");
    return Post(
        configuration.bQwenPrecomputeVoicePrompt ? "/prepare-voice" : "/prepare",
        configuration.bQwenPrecomputeVoicePrompt
            ? request.dump() : R"({"model":"clone"})");
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
    nlohmann::json request = VoiceRequest(text, preset);
    request["output_path"] = outputPath;
    return Post("/v1/audio/speech", request.dump());
}

VoiceOperationResult QwenTtsClient::SynthesizePcm(
    const std::string& text,
    const VoicePreset& preset)
{
    std::lock_guard lock(mutex);
    std::string error;
    if (!EnsureAvailable(error))
    {
        return {false, std::move(error), {}, -1.0};
    }
    return PostAudio("/v1/audio/pcm", VoiceRequest(text, preset).dump());
}

std::optional<std::vector<std::size_t>> ParseBatchClipSizes(
    const std::string& header,
    const std::size_t payloadBytes,
    const std::size_t expectedClips)
{
    if (header.empty() || expectedClips == 0)
    {
        return std::nullopt;
    }
    std::vector<std::size_t> sizes;
    std::size_t start = 0;
    while (start <= header.size())
    {
        const std::size_t comma = header.find(',', start);
        const std::string piece = comma == std::string::npos
            ? header.substr(start)
            : header.substr(start, comma - start);
        // An empty field means a trailing or doubled comma. Skipping it would silently
        // shift every later clip onto the wrong phrase.
        if (piece.empty())
        {
            return std::nullopt;
        }
        if (piece.find_first_not_of("0123456789") != std::string::npos)
        {
            return std::nullopt;
        }
        try
        {
            sizes.push_back(static_cast<std::size_t>(std::stoull(piece)));
        }
        catch (...)
        {
            return std::nullopt;
        }
        if (comma == std::string::npos)
        {
            break;
        }
        start = comma + 1;
    }
    if (sizes.size() != expectedClips)
    {
        return std::nullopt;
    }
    std::size_t total = 0;
    for (const std::size_t size : sizes)
    {
        // A zero-length clip is a phrase that produced no audio. Publishing it would
        // put a silent slot into ordered playback that never becomes audible.
        if (size == 0)
        {
            return std::nullopt;
        }
        total += size;
    }
    if (total != payloadBytes)
    {
        return std::nullopt;
    }
    return sizes;
}

std::vector<VoiceOperationResult> QwenTtsClient::SynthesizePcmBatch(
    const std::vector<std::string>& texts,
    const VoicePreset& preset)
{
    const auto failed = [&texts](std::string reason)
    {
        return std::vector<VoiceOperationResult>{
            {false, std::move(reason), {}, -1.0}};
    };
    if (texts.empty())
    {
        return failed("Batch synthesis was asked for no phrases.");
    }

    std::lock_guard lock(mutex);
    std::string error;
    if (!EnsureAvailable(error))
    {
        return failed(std::move(error));
    }

    nlohmann::json request = VoiceRequest(texts.front(), preset);
    request.erase("text");
    request["texts"] = texts;

    const auto requestStarted = std::chrono::steady_clock::now();
    httplib::Client client(configuration.qwenHost, configuration.qwenPort);
    client.set_connection_timeout(5);
    client.set_read_timeout(configuration.qwenRequestTimeoutSeconds);
    client.set_write_timeout(10);
    const httplib::Headers headers{{"Authorization", "Bearer " + apiKey}};
    const auto response = client.Post(
        "/v1/audio/pcm-batch", headers, request.dump(), "application/json");
    const double wallMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - requestStarted).count();

    if (!response)
    {
        return failed("The Qwen3-TTS batch worker did not return a response.");
    }
    if (response->status != 200)
    {
        return failed("Qwen3-TTS batch synthesis returned HTTP " +
            std::to_string(response->status) + ".");
    }
    // A JSON body here is the worker declining the batch -- most often because the
    // projected peak would not fit beside what is already on the card. That is a normal
    // answer, and the caller responds by synthesizing the phrases one at a time.
    if (response->get_header_value("Content-Type").find("application/json") !=
        std::string::npos)
    {
        try
        {
            const auto declined = nlohmann::json::parse(response->body);
            return failed(declined.value("message", "The batch was declined."));
        }
        catch (...)
        {
            return failed("The batch was declined without a readable reason.");
        }
    }

    // Clip lengths arrive in a header rather than inline, so the body stays one
    // contiguous read. Parsing them is the only thing standing between a framing bug
    // and audio played against the wrong phrase, so every mismatch is fatal to the
    // batch rather than repaired.
    const std::optional<std::vector<std::size_t>> parsed = ParseBatchClipSizes(
        response->get_header_value("X-Revia-Batch-Sizes"),
        response->body.size(),
        texts.size());
    if (!parsed.has_value())
    {
        return failed("The batch reply's clip framing did not match its payload.");
    }
    const std::vector<std::size_t>& sizes = *parsed;

    std::vector<double> clipDurations;
    {
        const std::string raw = response->get_header_value("X-Revia-Batch-Durations-Ms");
        std::size_t start = 0;
        while (start <= raw.size() && !raw.empty())
        {
            const std::size_t comma = raw.find(',', start);
            const std::string piece = raw.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            if (piece.empty()) break;
            try { clipDurations.push_back(std::stod(piece)); } catch (...) {}
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    // Every clip carries the whole batch's conditions. The generation time is the
    // batch's, not the clip's -- one call produced all of them, and splitting that
    // figure between clips would invent a per-phrase cost that was never paid.
    std::vector<VoiceOperationResult> results;
    results.reserve(texts.size());
    std::size_t offset = 0;
    for (std::size_t index = 0; index < sizes.size(); ++index)
    {
        VoiceOperationResult result;
        result.succeeded = true;
        result.message = "Qwen3-TTS returned batched phrase " +
            std::to_string(index + 1) + " of " + std::to_string(sizes.size()) + ".";
        result.elapsedMilliseconds = HeaderDouble(*response, "X-Revia-Elapsed-Ms");
        result.workerQueueMilliseconds = HeaderDouble(*response, "X-Revia-Queue-Ms");
        result.modelReadyMilliseconds = HeaderDouble(*response, "X-Revia-Model-Ready-Ms");
        result.clonePromptMilliseconds = HeaderDouble(*response, "X-Revia-Prompt-Ms");
        result.generationMilliseconds = HeaderDouble(*response, "X-Revia-Generation-Ms");
        result.wavWriteMilliseconds = HeaderDouble(*response, "X-Revia-Wav-Write-Ms");
        result.audioDurationMilliseconds = index < clipDurations.size()
            ? clipDurations[index]
            : -1.0;
        result.sampleRate = HeaderInt(*response, "X-Revia-Sample-Rate");
        result.clonePromptCached = HeaderInt(*response, "X-Revia-Prompt-Cached") != 0;
        result.device = response->get_header_value("X-Revia-Device");
        result.deviceName = response->get_header_value("X-Revia-Device-Name");
        result.dtype = response->get_header_value("X-Revia-Dtype");
        result.attentionBackend = response->get_header_value("X-Revia-Attention");
        result.inputMode = response->get_header_value("X-Revia-Input-Mode");
        result.vramUsedMiB = HeaderInt(*response, "X-Revia-Vram-Used-Mib");
        result.vramTotalMiB = HeaderInt(*response, "X-Revia-Vram-Total-Mib");
        result.peakVramMiB = HeaderInt(*response, "X-Revia-Peak-Vram-Mib");
        result.gpuUtilizationPercent = HeaderInt(*response, "X-Revia-Gpu-Utilization");
        result.modelResident = HeaderInt(*response, "X-Revia-Model-Resident") > 0;
    result.backend = response->get_header_value("X-Revia-Backend");
    result.cudaGraph = HeaderInt(*response, "X-Revia-Cuda-Graph") > 0;
    result.talkerGraph = HeaderInt(*response, "X-Revia-Talker-Graph") > 0;
        result.realTimeFactor = HeaderDouble(*response, "X-Revia-Real-Time-Factor");
        result.batchedPhrases = static_cast<int>(sizes.size());
        result.batchCharacters = HeaderInt(*response, "X-Revia-Batch-Characters");
        result.cppResponseMilliseconds = wallMilliseconds;
        result.audioBytes.assign(
            response->body.begin() + static_cast<std::ptrdiff_t>(offset),
            response->body.begin() + static_cast<std::ptrdiff_t>(offset + sizes[index]));
        offset += sizes[index];
        results.push_back(std::move(result));
    }
    return results;
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
    const auto requestStarted = std::chrono::steady_clock::now();
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
        result.attentionBackend = data.value("attention_backend", "");
        result.inputMode = data.value("input_mode", "");
        result.workerQueueMilliseconds = data.value("worker_queue_wait_ms", -1.0);
        result.modelReadyMilliseconds = data.value("model_ready_ms", -1.0);
        result.clonePromptMilliseconds = data.value("clone_prompt_ms", -1.0);
        result.generationMilliseconds = data.value("generation_ms", -1.0);
        result.wavWriteMilliseconds = data.value("wav_write_ms", -1.0);
        result.audioDurationMilliseconds = data.value("audio_duration_ms", -1.0);
        result.vramUsedMiB = data.value("vram_used_mib", -1);
        result.vramTotalMiB = data.value("vram_total_mib", -1);
        result.gpuUtilizationPercent = data.value("gpu_utilization", -1);
        result.modelResident = data.value("model_resident", false);
        result.backend = data.value("backend", "");
        result.cudaGraph = data.value("cuda_graph", false);
        result.talkerGraph = data.value("talker_graph", false);
        // Defaults chosen so an older worker that does not report these reads as "not
        // installed" rather than as installed. The claim that costs something is the
        // optimistic one.
        result.lowLatencyInstalled = data.value("low_latency_installed", false);
        result.backendDetail = data.value("low_latency_detail", "");
        result.realTimeFactor = data.value("real_time_factor", -1.0);
        result.sampleRate = data.value("sample_rate", 0);
        result.clonePromptCached = data.value("clone_prompt_cached", false);
        result.audioCacheHit = data.value("audio_cache_hit", false);
        result.cppResponseMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - requestStarted).count();
        return result;
    }
    catch (const std::exception& exception)
    {
        return {false, std::string("Qwen3-TTS returned invalid JSON: ") + exception.what(), {}, -1.0};
    }
}

VoiceOperationResult QwenTtsClient::PostAudio(
    const std::string& endpoint,
    const std::string& body)
{
    const auto requestStarted = std::chrono::steady_clock::now();
    httplib::Client client(configuration.qwenHost, configuration.qwenPort);
    client.set_connection_timeout(5);
    client.set_read_timeout(configuration.qwenRequestTimeoutSeconds);
    client.set_write_timeout(10);
    httplib::Headers headers{{"Authorization", "Bearer " + apiKey}};
    const auto response = client.Post(endpoint, headers, body, "application/json");
    const double wallMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - requestStarted).count();
    if (!response)
    {
        return {false, "The Qwen3-TTS PCM worker did not return a response.", {}, -1.0};
    }
    if (response->status != 200 ||
        response->get_header_value("Content-Type").find("audio/wav") == std::string::npos)
    {
        try
        {
            const auto error = nlohmann::json::parse(response->body);
            return {false, error.value("message", "Qwen3-TTS PCM synthesis failed."),
                {}, wallMilliseconds};
        }
        catch (...)
        {
            return {false, "Qwen3-TTS PCM synthesis returned HTTP " +
                std::to_string(response->status) + ".", {}, wallMilliseconds};
        }
    }

    VoiceOperationResult result;
    result.succeeded = true;
    result.message = "Qwen3-TTS returned an in-memory conversational audio buffer.";
    result.elapsedMilliseconds = HeaderDouble(*response, "X-Revia-Elapsed-Ms");
    result.workerQueueMilliseconds = HeaderDouble(*response, "X-Revia-Queue-Ms");
    result.modelReadyMilliseconds = HeaderDouble(*response, "X-Revia-Model-Ready-Ms");
    result.clonePromptMilliseconds = HeaderDouble(*response, "X-Revia-Prompt-Ms");
    result.generationMilliseconds = HeaderDouble(*response, "X-Revia-Generation-Ms");
    result.wavWriteMilliseconds = HeaderDouble(*response, "X-Revia-Wav-Write-Ms");
    result.audioDurationMilliseconds = HeaderDouble(
        *response, "X-Revia-Audio-Duration-Ms");
    result.sampleRate = HeaderInt(*response, "X-Revia-Sample-Rate");
    result.clonePromptCached = HeaderInt(*response, "X-Revia-Prompt-Cached") != 0;
    result.audioCacheHit = HeaderInt(*response, "X-Revia-Audio-Cache-Hit") != 0;
    result.device = response->get_header_value("X-Revia-Device");
    result.deviceName = response->get_header_value("X-Revia-Device-Name");
    result.dtype = response->get_header_value("X-Revia-Dtype");
    result.attentionBackend = response->get_header_value("X-Revia-Attention");
    result.vramUsedMiB = HeaderInt(*response, "X-Revia-Vram-Used-Mib");
    result.vramTotalMiB = HeaderInt(*response, "X-Revia-Vram-Total-Mib");
    result.gpuUtilizationPercent = HeaderInt(*response, "X-Revia-Gpu-Utilization");
    result.modelResident = HeaderInt(*response, "X-Revia-Model-Resident") > 0;
    result.backend = response->get_header_value("X-Revia-Backend");
    result.cudaGraph = HeaderInt(*response, "X-Revia-Cuda-Graph") > 0;
    result.talkerGraph = HeaderInt(*response, "X-Revia-Talker-Graph") > 0;
    result.realTimeFactor = HeaderDouble(*response, "X-Revia-Real-Time-Factor");
    result.inputMode = response->get_header_value("X-Revia-Input-Mode");
    result.cppResponseMilliseconds = wallMilliseconds;
    result.audioBytes.assign(response->body.begin(), response->body.end());
    return result;
}

} // namespace revia::speech
