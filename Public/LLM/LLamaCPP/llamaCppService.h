#pragma once

#include "Library/structLibrary.h"
#include "LLM/inferenceScheduler.h"
#include <atomic>
#include <functional>
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
    // Runs one real chat-template request so CUDA graph/JIT setup is paid during
    // startup instead of delaying the user's first turn.
    bool WarmUp(std::stop_token stopToken, std::string& outError) const;
    // Revia's own response posture for the next turn, already formatted. Stored rather
    // than threaded through every call because it changes per turn while the rest of the
    // request shape does not.
    void SetPosture(std::string posture);

    healthOutput CheckHealth() const;
    // onDelta receives visible text as it is generated, so a caller can begin speaking
    // the first sentence while the rest is still being produced.
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
    static std::string ParseStreamChunk(const std::string& line);
    int ResponseTokenLimit() const;
    // Shared by both planners: same low temperature, same JSON-object response format,
    // different contract and token ceiling.
    // structuredJson forces the server's JSON object mode. A diagram turns it off: the
    // payload is SVG, and making a small local model escape a whole document into a JSON
    // string burns most of the token budget on backslashes and fails on the first one it
    // gets wrong.
    responseOutput GeneratePlannerResponse(
        const std::string& systemPrompt,
        const std::string& userRequest,
        int maxTokens,
        bool structuredJson = true,
        std::stop_token stopToken = {},
        revia::llm::InferencePriority priority = revia::llm::InferencePriority::Interactive,
        float requestTemperature = 0.1F,
        const std::string& operation = "structured planning") const;

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
    mutable std::mutex postureMutex;
    std::string activePosture;
    mutable revia::llm::InferenceScheduler inferenceScheduler;
};
