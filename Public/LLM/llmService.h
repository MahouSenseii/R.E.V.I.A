#pragma once

#include "Library/enumLibrary.h"
#include "Library/structLibrary.h"
#include "LLM/LLamaCPP/llamaCppService.h"
#include <string>
#include <vector>

class llmService
{
public:
    llmService();
    ~llmService();

    void ApplySettings(const llmSettings& settings, const aiProfile& profile);
    bool IsBackendAvailable() const;
    healthOutput CheckBackendHealth() const;
    responseOutput GenerateResponse(const std::vector<conversationMessage> &context) const;
private:
    responseOutput GeneratePlaceholderResponse(const std::vector<conversationMessage>& context) const;
    llmBackendType backendType = llmBackendType::None;
    llamaCppService llamaCpp;
    bool bIsReady = false;
};
