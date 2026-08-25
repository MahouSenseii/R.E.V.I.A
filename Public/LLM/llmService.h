#pragma once

#include <functional>
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
    bool WarmUp(std::stop_token stopToken, std::string& outError) const;
    void SetPosture(std::string posture);
    healthOutput CheckBackendHealth() const;
    using DeltaHandler = std::function<void(const std::string&)>;
    responseOutput GenerateResponse(
        const std::vector<conversationMessage>& context,
        std::stop_token stopToken = {},
        DeltaHandler onDelta = {},
        bool forceDeepReasoning = false) const;
    responseOutput GenerateActionProposal(const std::string& userRequest) const;
    responseOutput ReviewConversationReply(
        const std::string& userInput,
        const std::string& candidateReply,
        const std::string& runtimeGroundTruth,
        int maxReviewTokens,
        std::stop_token stopToken = {}) const;
    responseOutput GenerateCuriosityPlan(
        const std::string& boundedContextPrompt,
        std::stop_token stopToken = {}) const;
    responseOutput GenerateGoalPlan(const std::string& userRequest) const;
    responseOutput GenerateDiagram(const std::string& userRequest) const;
    responseOutput ComposeContent(
        const std::string& request,
        const std::string& context) const;
    responseOutput ReviseBlock(
        const std::string& instruction,
        const std::string& neighbourhood,
        const std::string& target) const;
    responseOutput AnalyzeImage(
        const std::filesystem::path& imagePath,
        const std::string& prompt,
        int maxResponseTokens,
        std::stop_token stopToken = {}) const;
    memoryDecision EvaluateMemory(
        const std::string& userMessage,
        const std::string& assistantMessage = "",
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
