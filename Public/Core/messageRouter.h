#pragma once

#include <string>
#include <filesystem>
#include <stop_token>
#include <vector>

#include "LLM/llmService.h"

class messageRouter
{
public:
    messageRouter();
    ~messageRouter();

    responseOutput RouteMessage(
        const std::string& message,
        const std::vector<conversationMessage>& context,
        std::stop_token stopToken = {}) const;
    responseOutput PlanAction(const std::string& request) const;
    responseOutput AnalyzeImage(
        const std::filesystem::path& imagePath,
        const std::string& prompt,
        int maxResponseTokens,
        std::stop_token stopToken = {}) const;
    memoryDecision EvaluateMemory(
        const std::string& userMessage,
        std::stop_token stopToken = {}) const;
    bool IsLLMAvailable() const;
    healthOutput CheckLLMHealth() const;
    healthOutput CheckEmbeddingHealth() const;
    embeddingOutput EmbedMemory(
        const std::string& summary,
        std::stop_token stopToken = {}) const;
    void ApplyLLMSettings(
        const llmSettings& settings,
        const embeddingSettings& embeddingSettings,
        const aiProfile& profile);
    bool IsExitCommand(const std::string &input) const;

private:

    llmService llm;
};
