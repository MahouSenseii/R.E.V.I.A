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

    using DeltaHandler = std::function<void(const std::string&)>;
    responseOutput RouteMessage(
        const std::string& message,
        const std::vector<conversationMessage>& context,
        std::stop_token stopToken = {},
        DeltaHandler onDelta = {}) const;
    void SetPosture(std::string posture);
    responseOutput PlanAction(const std::string& request) const;
    responseOutput ReviewConversationReply(
        const std::string& userInput,
        const std::string& candidateReply,
        const std::string& runtimeGroundTruth,
        int maxReviewTokens,
        std::stop_token stopToken = {}) const;
    // Returns one structured curiosity nomination. It never executes the nominated
    // research or decides whether Revia may interrupt.
    responseOutput GenerateCuriosityPlan(
        const std::string& boundedContextPrompt,
        std::stop_token stopToken = {}) const;
    responseOutput PlanGoal(const std::string& request) const;
    responseOutput DrawDiagram(const std::string& request) const;
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
