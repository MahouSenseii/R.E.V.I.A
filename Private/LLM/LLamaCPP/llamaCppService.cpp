#include "LLM/LLamaCPP/llamaCppService.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <cctype>
#include <iostream>
#include <string>

using namespace std;
using json = nlohmann::json;

namespace
{
    constexpr const char* StopMarkers[] = {
        "<|im_end|>",
        "<|im_end>",
        "<|im_start|>",
        "<|eot_id|>",
        "<|end_of_text|>",
        "<|begin_of_text|>",
        "<|finetune_right_pad_id|>"
    };

    constexpr size_t StreamHoldbackChars = 32;

    size_t FindFirstStopMarker(const std::string& text)
    {
        size_t first = std::string::npos;

        for (const char* marker : StopMarkers)
        {
            const size_t pos = text.find(marker);
            if (pos != std::string::npos && (first == std::string::npos || pos < first))
            {
                first = pos;
            }
        }

        return first;
    }

    std::string TrimWhitespace(std::string text)
    {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        {
            text.erase(text.begin());
        }

        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        {
            text.pop_back();
        }

        return text;
    }

    std::string StripSpecialTokens(const std::string& text)
    {
        const size_t firstStop = FindFirstStopMarker(text);
        if (firstStop == std::string::npos)
        {
            return TrimWhitespace(text);
        }

        return TrimWhitespace(text.substr(0, firstStop));
    }
}

llamaCppService::llamaCppService() = default;

llamaCppService::~llamaCppService() = default;

void llamaCppService::ApplySettings(const llmSettings& settings, const aiProfile& profile)
{
    host = settings.host;
    port = settings.port;
    modelName = settings.modelName;

    activeProfile = profile;

    temperature = profile.bHasTemperatureOverride
        ? profile.temperature
        : settings.temperature;

    maxTokens = profile.bHasMaxTokensOverride
        ? profile.maxTokens
        : settings.maxTokens;
}

bool llamaCppService::IsServerAvailable() const
{
    return CheckHealth().bIsAvailable;
}

responseOutput llamaCppService::GenerateResponse(const std::vector<conversationMessage>& context) const
{
    responseOutput output;

    if (context.empty())
    {
        output.bSuccess = false;
        output.response = "I need something to respond to.";
        output.reason = "Conversation context was empty.";
        output.bShouldSpeak = true;
        output.bShouldRemember = false;
        return output;
    }

    httplib::Client client(host, port);
    client.set_connection_timeout(5);
    client.set_read_timeout(120);

    json requestBody;
    json messages = builder.BuildMessages(activeProfile, context);

    requestBody["model"]       = modelName;
    requestBody["messages"]    = messages;
    requestBody["temperature"] = temperature;
    requestBody["max_tokens"]  = maxTokens;
    requestBody["stream"]      = true;
    requestBody["stop"]        = json::array();
    for (const char* marker : StopMarkers)
    {
        requestBody["stop"].push_back(marker);
    }

    std::string fullResponse;
    std::string buffer;
    size_t printedLength = 0;

    httplib::Request req;
    req.method  = "POST";
    req.path    = "/v1/chat/completions";
    req.body    = requestBody.dump();
    req.set_header("Content-Type", "application/json");


    req.content_receiver = [&](const char* data, size_t length, uint64_t /*offset*/, uint64_t /*total*/) -> bool
    {
        buffer.append(data, length);

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos)
        {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.empty()) continue;
            if (line.rfind("data: ", 0) != 0) continue;

            const std::string jsonStr = line.substr(6);
            if (jsonStr == "[DONE]") continue;

            const std::string token = ParseStreamChunk(line);
            if (token.empty()) continue;

            fullResponse += token;

            const std::string visibleResponse = StripSpecialTokens(fullResponse);
            const size_t safeLength = visibleResponse.size() > StreamHoldbackChars
                ? visibleResponse.size() - StreamHoldbackChars
                : 0;

            if (safeLength > printedLength)
            {
                std::cout << visibleResponse.substr(printedLength, safeLength - printedLength) << std::flush;
                printedLength = safeLength;
            }
        }

        return true;
    };

    const auto result = client.send(req);

    const std::string cleanedResponse = StripSpecialTokens(fullResponse);
    if (cleanedResponse.size() > printedLength)
    {
        std::cout << cleanedResponse.substr(printedLength) << std::flush;
    }
    std::cout << "\n\n";

    if (!result)
    {
        output.bSuccess = false;
        output.response = "I could not connect to my local llama.cpp server.";
        output.reason   = "Failed to connect to llama.cpp server at " + host + ":" + std::to_string(port) + ".";
        output.bShouldSpeak    = true;
        output.bShouldRemember = false;
        return output;
    }

    if (result->status != 200)
    {
        output.bSuccess = false;
        output.response = "My local llama.cpp backend returned an error.";
        output.reason   = "llama.cpp server returned HTTP status: " + std::to_string(result->status);
        output.bShouldSpeak    = true;
        output.bShouldRemember = false;
        return output;
    }

    if (cleanedResponse.empty())
    {
        output.bSuccess = false;
        output.response = "The model returned an empty response.";
        output.reason   = "Stream completed but no usable content tokens were received.";
        output.bShouldSpeak    = true;
        output.bShouldRemember = false;
        return output;
    }

    output.bSuccess        = true;
    output.response        = cleanedResponse;
    output.bShouldSpeak    = true;
    output.bShouldRemember = false;
    output.bWasStreamed     = true;  // tokens already printed live

    return output;
}

std::string llamaCppService::ParseStreamChunk(const std::string &line)
{
    if (line.rfind("data: ", 0) != 0) return "";
    const std::string json_str = line.substr(6);

    if (json_str == "[DONE]") return "";
    try
    {
        const json chunk = json::parse(json_str);

        if (!chunk.contains("choices") || chunk["choices"].empty()) return "";

        const auto& delta = chunk["choices"][0]["delta"];

        if (!delta.contains("content")) return "";

        return delta["content"].get<std::string>();
    }
    catch (...) { return ""; }
}

healthOutput llamaCppService::CheckHealth() const
{
    healthOutput output;
    output.name = "llama.cpp";

    httplib::Client client(host, port);
    client.set_connection_timeout(3);
    client.set_read_timeout(5);

    const auto result = client.Get("/v1/models");

    if (!result)
    {
        output.bIsAvailable = false;
        output.status = systemStatus::Red;
        output.message = "llama.cpp server is offline.";
        output.reason = "Failed to connect to llama.cpp server at " + host + ":" + std::to_string(port) + ".";
        return output;
    }

    if (result->status != 200)
    {
        output.bIsAvailable = false;
        output.status = systemStatus::Red;
        output.message = "llama.cpp server responded with an error.";
        output.reason = "HTTP status: " + std::to_string(result->status);
        return output;
    }

    output.bIsAvailable = true;
    output.status = systemStatus::Green;
    output.message = "llama.cpp server is online.";
    output.reason = "";

    return output;
}
