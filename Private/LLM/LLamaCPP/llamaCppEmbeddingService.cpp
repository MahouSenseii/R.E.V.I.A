#include "LLM/LLamaCPP/llamaCppEmbeddingService.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace
{
    embeddingOutput FinishEmbedding(
        embeddingOutput output,
        const std::chrono::steady_clock::time_point started)
    {
        output.elapsedMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return output;
    }

    void ApplyApiKey(httplib::Client& client, const std::string& apiKey)
    {
        if (!apiKey.empty())
        {
            client.set_default_headers({{"Authorization", "Bearer " + apiKey}});
        }
    }
}

void llamaCppEmbeddingService::ApplySettings(const embeddingSettings& settings)
{
    enabled = settings.bEnabled;
    host = settings.host;
    port = settings.port;
    modelName = settings.modelName;
    apiKey = settings.apiKey;
    queryPrefix = settings.queryPrefix;
    documentPrefix = settings.documentPrefix;
}

bool llamaCppEmbeddingService::IsEnabled() const
{
    return enabled;
}

const std::string& llamaCppEmbeddingService::ModelName() const
{
    return modelName;
}

healthOutput llamaCppEmbeddingService::CheckHealth() const
{
    healthOutput output;
    output.name = "llama.cpp embeddings";
    if (!enabled)
    {
        output.status = systemStatus::Yellow;
        output.message = "Semantic memory is disabled.";
        output.reason = "The embedding service is disabled in Config/settings.json.";
        return output;
    }

    httplib::Client client(host, port);
    ApplyApiKey(client, apiKey);
    client.set_connection_timeout(2);
    client.set_read_timeout(3);
    const auto health = client.Get("/health");
    if (!health || health->status != 200)
    {
        output.status = systemStatus::Red;
        output.message = "The embedding server is offline.";
        output.reason = "Failed to reach the embedding server at " + host + ":" +
            std::to_string(port) + ".";
        return output;
    }

    const auto models = client.Get("/v1/models");
    if (!models || models->status != 200)
    {
        output.status = systemStatus::Yellow;
        output.message = "The embedding server is online, but its model could not be verified.";
        output.reason = "The embedding server did not expose /v1/models.";
        return output;
    }

    try
    {
        const json body = json::parse(models->body);
        bool foundModel = false;
        if (body.contains("data") && body["data"].is_array())
        {
            for (const auto& entry : body["data"])
            {
                if (entry.is_object())
                {
                    const std::string loadedName = entry.value("id", "");
                    if (loadedName == modelName ||
                        std::filesystem::path(loadedName).filename().string() ==
                            std::filesystem::path(modelName).filename().string())
                    {
                        foundModel = true;
                        break;
                    }
                }
            }
        }
        if (!foundModel)
        {
            output.status = systemStatus::Yellow;
            output.message = "The embedding server loaded a different model.";
            output.reason = "Expected embedding model " + modelName + ".";
            return output;
        }
    }
    catch (const std::exception& error)
    {
        output.status = systemStatus::Yellow;
        output.message = "The embedding server returned invalid model metadata.";
        output.reason = error.what();
        return output;
    }

    output.bIsAvailable = true;
    output.status = systemStatus::Green;
    output.message = "The semantic-memory embedding server is online.";
    return output;
}

embeddingOutput llamaCppEmbeddingService::EmbedQuery(
    const std::string& text,
    const std::stop_token stopToken) const
{
    return Embed(text, queryPrefix, stopToken);
}

embeddingOutput llamaCppEmbeddingService::EmbedDocument(
    const std::string& text,
    const std::stop_token stopToken) const
{
    return Embed(text, documentPrefix, stopToken);
}

embeddingOutput llamaCppEmbeddingService::Embed(
    const std::string& text,
    const std::string& prefix,
    const std::stop_token stopToken) const
{
    const auto started = std::chrono::steady_clock::now();
    embeddingOutput output;
    output.model = modelName;
    if (!enabled)
    {
        output.reason = "Semantic memory is disabled.";
        return FinishEmbedding(std::move(output), started);
    }
    if (text.empty())
    {
        output.reason = "Cannot embed empty text.";
        return FinishEmbedding(std::move(output), started);
    }

    httplib::Client client(host, port);
    ApplyApiKey(client, apiKey);
    client.set_connection_timeout(2);
    client.set_read_timeout(10);
    std::stop_callback cancelRequest(stopToken, [&client]()
    {
        client.stop();
    });
    if (stopToken.stop_requested())
    {
        output.reason = "Embedding request was cancelled.";
        return FinishEmbedding(std::move(output), started);
    }

    const json request = {
        {"model", modelName},
        {"input", prefix + text},
        {"encoding_format", "float"}
    };
    const auto result = client.Post("/v1/embeddings", request.dump(), "application/json");
    if (!result)
    {
        output.reason = stopToken.stop_requested()
            ? "Embedding request was cancelled."
            : "Embedding request failed: " +
                std::string(httplib::to_string(result.error())) + ".";
        return FinishEmbedding(std::move(output), started);
    }
    if (result->status != 200)
    {
        output.reason = "Embedding server returned HTTP status " +
            std::to_string(result->status) + ".";
        return FinishEmbedding(std::move(output), started);
    }

    try
    {
        const json body = json::parse(result->body);
        if (!body.contains("data") || !body["data"].is_array() || body["data"].empty() ||
            !body["data"][0].contains("embedding") ||
            !body["data"][0]["embedding"].is_array())
        {
            output.reason = "Embedding response did not contain data[0].embedding.";
            return FinishEmbedding(std::move(output), started);
        }

        output.values.reserve(body["data"][0]["embedding"].size());
        double squaredLength = 0.0;
        for (const auto& component : body["data"][0]["embedding"])
        {
            const float value = component.get<float>();
            if (!std::isfinite(value))
            {
                output.values.clear();
                output.reason = "Embedding response contained a non-finite value.";
                return FinishEmbedding(std::move(output), started);
            }
            output.values.push_back(value);
            squaredLength += static_cast<double>(value) * static_cast<double>(value);
        }

        if (output.values.empty() || squaredLength <= 0.0)
        {
            output.values.clear();
            output.reason = "Embedding response was empty or had zero length.";
            return FinishEmbedding(std::move(output), started);
        }

        const float inverseLength = static_cast<float>(1.0 / std::sqrt(squaredLength));
        for (float& value : output.values)
        {
            value *= inverseLength;
        }
        output.bSuccess = true;
        return FinishEmbedding(std::move(output), started);
    }
    catch (const std::exception& error)
    {
        output.values.clear();
        output.reason = std::string("Could not parse the embedding response: ") + error.what();
        return FinishEmbedding(std::move(output), started);
    }
}
