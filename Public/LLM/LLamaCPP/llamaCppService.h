#pragma once

#include "Library/structLibrary.h"
#include <atomic>
#include <filesystem>
#include <string>
#include <mutex>
#include <stop_token>
#include <vector>

#include "LLM/promptBuilder.h"
#include "LLM/LLamaCPP/llamaCppEmbeddingService.h"

class llamaCppService
{
public:
    llamaCppService();
    ~llamaCppService();

    void ApplySettings(
        const llmSettings& settings,
        const embeddingSettings& embeddingSettings,
        const aiProfile& profile);
    bool IsServerAvailable() const;

    healthOutput CheckHealth() const;
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
    static std::string ParseStreamChunk(const std::string& line);
    int ResponseTokenLimit() const;
    // Shared by both planners: same low temperature, same JSON-object response format,
    // different contract and token ceiling.
    responseOutput GeneratePlannerResponse(
        const std::string& systemPrompt,
        const std::string& userRequest,
        int maxTokens) const;

    std::string host = "127.0.0.1";
    int port = 8080;
    std::string modelName = "local-model";
    std::string apiKey;
    float temperature = 0.7f;
    bool bAutoMaxTokens = true;
    int maxTokens = 4096;
    bool bVisionExpected = false;
    mutable std::atomic<int> effectiveContextTokens = 0;
    mutable std::atomic<int> effectiveParallelSlots = 0;

    promptBuilder builder;
    llamaCppEmbeddingService embeddings;
    aiProfile activeProfile;
    mutable std::mutex inferenceMutex;
};
