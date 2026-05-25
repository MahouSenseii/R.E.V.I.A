#pragma once

#include "Library/structLibrary.h"
#include <string>
#include <vector>

#include "LLM/promptBuilder.h"

class llamaCppService
{
public:
    llamaCppService();
    ~llamaCppService();

    void ApplySettings(const llmSettings& settings, const aiProfile& profile);
    bool IsServerAvailable() const;

    healthOutput CheckHealth() const;
    responseOutput GenerateResponse(const std::vector<conversationMessage>& context) const;

private:
    static responseOutput ParseResponseBody(const std::string& responseBody);

    std::string host = "127.0.0.1";
    int port = 8080;
    std::string modelName = "local-model";
    float temperature = 0.7f;
    int maxTokens = 512;

    promptBuilder builder;
    aiProfile activeProfile;
};
