#include "LLM/LLamaCPP/llamaCppService.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <exception>
#include <string>

using json = nlohmann::json;

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
    // L9: single source of truth for the connectivity probe.
    return CheckHealth().bIsAvailable;
}

responseOutput llamaCppService::GenerateResponse(
    const std::vector<conversationMessage>& context
) const
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

    requestBody["model"] = modelName;
    requestBody["messages"] = messages;
    requestBody["temperature"] = temperature;
    requestBody["max_tokens"] = maxTokens;
    requestBody["stream"] = false;

    const auto result = client.Post(
        "/v1/chat/completions",
        requestBody.dump(),
        "application/json"
    );

    if (!result)
    {
        output.bSuccess = false;
        output.response = "I could not connect to my local llama.cpp server.";
        output.reason = "Failed to connect to llama.cpp server at " + host + ":" + std::to_string(port) + ".";
        output.bShouldSpeak = true;
        output.bShouldRemember = false;

        return output;
    }

    if (result->status != 200)
    {
        output.bSuccess = false;
        output.response = "My local llama.cpp backend returned an error.";
        output.reason = "llama.cpp server returned HTTP status: " + std::to_string(result->status);
        output.bShouldSpeak = true;
        output.bShouldRemember = false;

        return output;
    }

    return ParseResponseBody(result->body);
}

responseOutput llamaCppService::ParseResponseBody(const std::string &responseBody) {
    responseOutput output;

    try
    {
        const json parsedBody = json::parse(responseBody);

        if (!parsedBody.contains("choices") || parsedBody["choices"].empty())
        {
            output.bSuccess = false;
            output.response = "The language model returned no response.";
            output.reason = "llama.cpp response did not contain choices.";
            output.bShouldSpeak = true;
            output.bShouldRemember = false;

            return output;
        }

        if (!parsedBody["choices"][0].contains("message") ||
            !parsedBody["choices"][0]["message"].contains("content"))
        {
            output.bSuccess = false;
            output.response = "The language model response format was unexpected.";
            output.reason = "llama.cpp response did not contain choices[0].message.content.";
            output.bShouldSpeak = true;
            output.bShouldRemember = false;

            return output;
        }

        output.bSuccess = true;
        output.response = parsedBody["choices"][0]["message"]["content"].get<std::string>();
        output.reason = "";
        output.bShouldSpeak = true;
        output.bShouldRemember = false;

        return output;
    }
    catch (const std::exception& error)
    {
        output.bSuccess = false;
        output.response = "I had trouble reading the llama.cpp response.";
        output.reason = std::string("Failed to parse llama.cpp response: ") + error.what();
        output.bShouldSpeak = true;
        output.bShouldRemember = false;

        return output;
    }
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
