#pragma once

#include "Library/enumLibrary.h"
#include "Library/structLibrary.h"
#include "LLM/LLamaCPP/llamaCppService.h"
#include <string>
#include <filesystem>
#include <stop_token>
#include <vector>

class llmService
{
public:
    llmService();
    ~llmService();

    void ApplySettings(
        const llmSettings& settings,
        const embeddingSettings& embeddingSettings,
        const aiProfile& profile);
    bool IsBackendAvailable() const;
    healthOutput CheckBackendHealth() const;
    responseOutput GenerateResponse(
        const std::vector<conversationMessage>& context,
        std::stop_token stopToken = {}) const;
    responseOutput GenerateActionProposal(const std::string& userRequest) const;
    responseOutput GenerateGoalPlan(const std::string& userRequest) const;
    responseOutput AnalyzeImage(
        const std::filesystem::path& imagePath,
        const std::string& prompt,
        int maxResponseTokens,
        std::stop_token stopToken = {}) const;
    memoryDecision EvaluateMemory(
        const std::string& userMessage,
        std::stop_token stopToken = {}) const;
    healthOutput CheckEmbeddingHealth() const;
    embeddingOutput EmbedMemory(
        const std::string& summary,
        std::stop_token stopToken = {}) const;
private:
    responseOutput GeneratePlaceholderResponse(const std::vector<conversationMessage>& context) const;
    llmBackendType backendType = llmBackendType::None;
    llamaCppService llamaCpp;
    bool bIsReady = false;
};
