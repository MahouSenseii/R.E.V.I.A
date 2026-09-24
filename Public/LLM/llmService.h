#pragma once

#include "Agents/responseProvenance.h"
#include <functional>
#include "Library/enumLibrary.h"
#include "Library/structLibrary.h"
#include "LLM/LLamaCPP/llamaCppService.h"
#include "LLM/privateMemoryAccess.h"
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
    void ApplyProfile(const llmSettings& settings, const aiProfile& profile);
    bool IsBackendAvailable(std::stop_token stopToken = {}) const;
    bool WarmUp(std::stop_token stopToken, std::string& outError) const;
    void SetPosture(std::string posture);
    void SetReplyNote(std::string note);
    std::string RelatedMemories(const std::string& query, std::stop_token stopToken = {}) const;
    healthOutput CheckBackendHealth(std::stop_token stopToken = {}) const;
    using DeltaHandler = std::function<void(const std::string&)>;
    responseOutput GenerateResponse(
        const std::vector<conversationMessage>& context,
        std::stop_token stopToken = {},
        DeltaHandler onDelta = {},
        bool deepReasoning = false,
        revia::llm::PrivateMemoryAccess memoryAccess = revia::llm::PrivateMemoryAccess::ProfileSetting) const;
    responseOutput GenerateActionProposal(const std::string& userRequest) const;
    responseOutput ReviewConversationReply(
        const std::string& userInput,
        const std::string& candidateReply,
        const std::string& runtimeGroundTruth,
        int maxReviewTokens,
        std::stop_token stopToken = {}) const;
    responseOutput GenerateActivityDraft(const std::string& topic,
        const std::string& context, std::stop_token stopToken = {}) const;
    responseOutput GenerateCuriosityPlan(
        const std::string& boundedContextPrompt,
        const std::vector<std::string>& availableActions,
        std::stop_token stopToken = {}) const;
    responseOutput Deliberate(
        const std::string& boundedInquiryPrompt,
        std::stop_token stopToken = {}) const;
    responseOutput GenerateGoalPlan(const std::string& userRequest) const;
    responseOutput GenerateCodeReview(
        const std::string& instructions,
        const std::string& material,
        const std::string& schema,
        std::stop_token stopToken = {}) const;
    responseOutput GenerateComputerSubgoal(
        const std::string& instruction,
        const std::string& situation,
        const std::string& schema,
        std::stop_token stopToken = {}) const;

    responseOutput GenerateNextGoalStep(
        const std::string& goalContext,
        std::stop_token stopToken = {}) const;
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
        std::stop_token stopToken = {},
        bool backgroundAwareness = false) const;
    // `provenance` says whether the assistant text is Revia speaking for herself. It
    // has no default on purpose: a caller that does not know must decide, because the
    // safe answer and the convenient answer are not the same one.
    memoryDecision EvaluateMemory(
        const std::string& userMessage,
        const std::string& assistantMessage,
        revia::agents::ResponseProvenance provenance,
        std::stop_token stopToken = {}) const;
    healthOutput CheckEmbeddingHealth(std::stop_token stopToken = {}) const;
    embeddingOutput EmbedMemory(
        const std::string& summary,
        std::stop_token stopToken = {}) const;
private:
    responseOutput GeneratePlaceholderResponse(const std::vector<conversationMessage>& context) const;
    llmBackendType backendType = llmBackendType::None;
    llamaCppService llamaCpp;
    bool bIsReady = false;
};
