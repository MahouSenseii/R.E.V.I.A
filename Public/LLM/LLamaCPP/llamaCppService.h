#pragma once

#include "Agents/responseProvenance.h"
#include "Library/structLibrary.h"
#include "LLM/inferenceScheduler.h"
#include <atomic>
#include <cstddef>
#include <functional>
#include <filesystem>
#include <optional>
#include <string>
#include <mutex>
#include <stop_token>
#include <unordered_map>
#include <vector>

#include "LLM/promptBuilder.h"
#include "LLM/privateMemoryAccess.h"
#include "LLM/providerCapabilities.h"
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
    void ApplyProfile(const llmSettings& settings, const aiProfile& profile);
    bool IsServerAvailable(std::stop_token stopToken = {}) const;
    // Runs one real chat-template request so CUDA graph/JIT setup is paid during
    // startup instead of delaying the user's first turn.
    bool WarmUp(std::stop_token stopToken, std::string& outError) const;
    // Revia's own response posture for the next turn, already formatted. Stored rather
    // than threaded through every call because it changes per turn while the rest of the
    // request shape does not.
    void SetPosture(std::string posture);
    void SetReplyNote(std::string note);
    // Kept in the system message, after the profile prompt, because it changes only
    // when the record does: the prefix stays cacheable between turns.
    void SetStableContext(std::string context);

    healthOutput CheckHealth(std::stop_token stopToken = {}) const;
    // onDelta receives visible text as it is generated, so a caller can begin speaking
    // the first sentence while the rest is still being produced.
    using DeltaHandler = std::function<void(const std::string&)>;
    responseOutput GenerateResponse(
        const std::vector<conversationMessage>& context,
        std::stop_token stopToken = {},
        DeltaHandler onDelta = {},
        bool deepReasoning = false,
        revia::llm::PrivateMemoryAccess memoryAccess = revia::llm::PrivateMemoryAccess::ProfileSetting) const;
    // `toolCatalog`: the MCP tools the planner may name, with their pinned
    // descriptions (Skills/mcpRegistry.h), or empty when none is offered.
    responseOutput GenerateActionProposal(
        const std::string& userRequest, const std::string& toolCatalog = {}) const;
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
    // Returns the questions Revia is putting to herself about a hard turn. It never
    // produces the visible reply; the conversational model still generates that.
    responseOutput Deliberate(
        const std::string& boundedInquiryPrompt,
        std::stop_token stopToken = {}) const;
    // The observer and reflector passes over the conversation record. Background
    // priority.
    responseOutput ObserveConversation(
        const std::string& boundedHistory,
        std::stop_token stopToken = {}) const;
    responseOutput ReflectOnConversation(
        const std::string& boundedRecord,
        std::stop_token stopToken = {}) const;
    // The quarantined reader over looked-up pages (Internet/webReader.h): a bounded
    // call with no tools whose only output is claims tied to numbered sources.
    // Interactive priority, because the reply is waiting on it.
    responseOutput ReadWebPages(
        const std::string& boundedEnvelope,
        std::stop_token stopToken = {}) const;
    // Inner Thoughts (Initiative/innerThoughts.h): three numbers on a thought she is
    // considering saying. Background priority: a conversation turn preempts it, and
    // an unscored thought simply stays a thought.
    responseOutput ScoreInnerThought(
        const std::string& thoughtEnvelope,
        std::stop_token stopToken = {}) const;
    // One move in a game (Games/gamePlayer.h), under a schema that only admits the
    // actions the game allows. Interactive: the game is waiting.
    responseOutput PlanGameAction(
        const std::string& envelope,
        const std::string& schema,
        std::stop_token stopToken = {}) const;
    responseOutput GenerateGoalPlan(const std::string& userRequest) const;
    // One step of an iterative run, decided from the attempts so far.
    // One bounded subgoal, under its own grammar.
    //
    // The whole instruction is the system prompt here, not a user message wrapped in
    // the next-step planner's prompt. Routing it through that one meant the step
    // grammar decided the answer's shape and the subgoal instructions were ignored --
    // which no scripted test could see, because a scripted test supplies the answer.
    // One structured review of her own code. Background priority: a conversation turn
    // preempts it.
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
    // The saved memories nearest `query`, as the prompt block the reply would carry.
    // Empty when the profile has memory switched off.
    std::string RelatedMemories(
        const std::string& query,
        std::stop_token stopToken = {}) const;
    embeddingOutput EmbedMemory(
        const std::string& summary,
        std::stop_token stopToken = {}) const;

private:
    static std::string ParseStreamChunk(
        const std::string& line,
        std::string* outFinishReason = nullptr);
    int ResponseTokenLimit() const;
    // The model's own token count for `text`, from llama-server's /tokenize, remembered
    // so the history repeated every turn is counted once. Empty when the server cannot
    // say; the caller then spends the byte estimate, which is safe but costs history.
    std::optional<std::size_t> CountTokens(
        const std::string& text,
        std::stop_token stopToken) const;
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
        const std::string& operation = "structured planning",
        const std::string& responseSchema = {}) const;

    // Which server this is, and so which requests it can take. Everything llama.cpp-
    // specific is sent only when the capabilities say the server understands it.
    revia::llm::ProviderCapabilities capabilities;
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string modelName = "local-model";
    std::string apiKey;
    float temperature = 0.7f;
    float xtcProbability = 0.0f;
    float xtcThreshold = 0.1f;
    bool bAutoMaxTokens = true;
    bool bStablePromptPrefix = true;
    int maxTokens = 4096;
    bool bVisionExpected = false;
    int configuredContextTokens = 4096;
    mutable std::atomic<int> effectiveContextTokens = 0;
    mutable std::atomic<int> effectiveParallelSlots = 0;

    promptBuilder builder;
    llamaCppEmbeddingService embeddings;
    aiProfile activeProfile;
    mutable std::mutex postureMutex;
    std::string activePosture;
    std::string activeReplyNote;
    std::string activeStableContext;
    mutable revia::llm::InferenceScheduler inferenceScheduler;
    // Keyed by a hash of the text and its length. Cleared with the settings, because a
    // different model is a different tokenizer.
    mutable std::mutex tokenCountMutex;
    mutable std::unordered_map<std::size_t, std::size_t> tokenCounts;
    // Set when the server answers /tokenize with 404, so a backend without it is not
    // asked once per message on every turn.
    mutable std::atomic<bool> bTokenizerUnavailable = false;
};
