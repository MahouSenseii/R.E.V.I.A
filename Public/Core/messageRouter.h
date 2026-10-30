#pragma once

#include "Core/conversationMessage.h"
#include "Core/profile.h"
#include "LLM/backendTypes.h"
#include "LLM/embeddingTypes.h"
#include "LLM/endpointSettings.h"
#include "LLM/responseTypes.h"
#include "LLM/codeReviewPurpose.h"
#include "Memory/memoryTypes.h"
#include "Agents/responseProvenance.h"
#include <mutex>
#include <optional>
#include <string>
#include <filesystem>
#include <stop_token>
#include <vector>

#include "LLM/llmService.h"
#include "LLM/privateMemoryAccess.h"
#include "LLM/Cloud/cloudChatService.h"
#include "Intelligence/intelligenceTypes.h"
#include "Intelligence/modelLifetime.h"
#include "Intelligence/modelResidencyManager.h"

class messageRouter
{
public:
    messageRouter();
    explicit messageRouter(std::string memoryDatabasePath);
    ~messageRouter();
    embeddingOutput EmbedMemoryQuery(const std::string& query, std::stop_token stopToken = {}) const
    { return llm.EmbedMemoryQuery(query, stopToken); }
    void SetMemoryReranker(const std::shared_ptr<revia::memory::MemoryReranker>& value)
    {
        llm.SetMemoryReranker(value);
        fastLlm.SetMemoryReranker(value);
        expertLlm.SetMemoryReranker(value);
    }

    using DeltaHandler = std::function<void(const std::string&)>;
    // `tools` offers this turn's tool definitions and replays its earlier tool steps. While
    // it is set, the reasoning handler is kept for the turn's later steps instead of being
    // used once.
    responseOutput RouteMessage(const std::string& message, const std::vector<conversationMessage>& context, std::stop_token stopToken = {},
        DeltaHandler onDelta = {}, const revia::intelligence::IntelligenceDecision& decision = {},
        revia::llm::PrivateMemoryAccess memoryAccess = revia::llm::PrivateMemoryAccess::ProfileSetting,
        const revia::llm::ToolExchange* tools = nullptr) const;
    // SetPosture clears the reasoning handler with the rest of the turn's state.
    void SetPosture(std::string posture);
    using ReasoningHandler = llmService::ReasoningHandler;
    // Receives this turn's thinking as it streams. Used by the next RouteMessage only.
    void SetReasoningHandler(ReasoningHandler handler);
    // Set after SetPosture, which clears it: the lines this turn's answer must follow,
    // placed at the end of the prompt rather than in the system message.
    void SetReplyNote(std::string note);
    // Applies to local source explanations until SetPosture resets the turn state.
    void SetSourceExplanation(bool enabled);
    // Set after SetPosture, which clears it: asks the next RouteMessage to try the
    // optional cloud model. An explicit request is answered by the cloud or refused; an
    // automatic one (a hard turn) falls back to the local route when the cloud says no.
    void SetCloudTurn(revia::llm::cloud::CloudTurnRequest request);
    // The cloud backend's own switches and status, for /cloud and the session wiring.
    [[nodiscard]] revia::llm::cloud::CloudChatService& Cloud()
    { return cloud; }
    [[nodiscard]] const revia::llm::cloud::CloudChatService& Cloud() const
    { return cloud; }
    // The saved memories related to `query`, for work that happens before the reply's
    // own prompt is built -- the self-inquiry.
    std::string RelatedMemories(
        const std::string& query, std::stop_token stopToken = {}, const revia::memory::MemoryScope& scope = {}) const;
    responseOutput PlanAction(const std::string& request) const;
    // A review of her own code. Expert when it can be had -- loaded on demand if it is
    // managed that way -- because this is the hardest reading she does and nobody is
    // waiting on it; Main otherwise.
    responseOutput ReviewCode(const std::string& instructions, const std::string& material, const std::string& schema,
        std::stop_token stopToken = {}, revia::llm::CodeReviewPurpose purpose = revia::llm::CodeReviewPurpose::Development) const;
    responseOutput ReviewConversationReply(const std::string& userInput, const std::string& candidateReply,
        const std::string& runtimeGroundTruth, int maxReviewTokens, std::stop_token stopToken = {},
        const std::string& conversationEvidence = {}) const;
    // Returns one structured curiosity nomination. It never executes the nominated
    // research or decides whether Revia may interrupt.
    responseOutput GenerateActivityDraft(const std::string& topic, const std::string& context, std::stop_token stopToken = {}) const;
    responseOutput GenerateCuriosityPlan(const std::string& boundedContextPrompt,
        const std::vector<std::string>& availableActions, std::stop_token stopToken = {}) const;
    // Runs one bounded self-inquiry pass for a hard conversational turn. Interactive
    // priority, because the user's own reply is waiting behind it.
    responseOutput Deliberate(const std::string& boundedInquiryPrompt, std::stop_token stopToken = {}) const;
    responseOutput ProposeCalculation(const std::string& envelope, std::stop_token stopToken = {}) const;
    responseOutput PlanGoal(const std::string& request) const;
    // The iterative form: one step at a time, from what has already happened.
    // Ask Main for one bounded subgoal.
    //
    // A separate entry point from PlanNextGoalStep and not a convenience: the two ask
    // different questions under different grammars, and sharing one made the subgoal
    // question unanswerable.
    responseOutput PlanComputerSubgoal(const std::string& instruction,
        const std::string& situation, const std::string& schema, std::stop_token stopToken = {}) const;

    responseOutput PlanNextGoalStep(const std::string& goalContext, std::stop_token stopToken = {}) const;
    responseOutput DrawDiagram(const std::string& request) const;
    responseOutput ComposeContent(const std::string& request, const std::string& context) const;
    responseOutput ReviseBlock(const std::string& instruction, const std::string& neighbourhood, const std::string& target) const;
    responseOutput AnalyzeImage(const std::filesystem::path& imagePath, const std::string& prompt, int maxResponseTokens,
        std::stop_token stopToken = {}, bool backgroundAwareness = false) const;
    // `provenance` says whether the assistant text is Revia speaking for herself. It
    // has no default on purpose: a caller that does not know must decide, because the
    // safe answer and the convenient answer are not the same one.
    memoryDecision EvaluateMemory(const std::string& userMessage, const std::string& assistantMessage,
        revia::agents::ResponseProvenance provenance, std::stop_token stopToken = {}, const revia::memory::MemoryScope& scope = {}) const;
    bool IsLLMAvailable() const;
    bool WarmUpLLM(std::stop_token stopToken, std::string& outError) const;
    bool WarmUpFast(std::stop_token stopToken, std::string& outError) const;
    bool WarmUpExpert(std::stop_token stopToken, std::string& outError) const;
    healthOutput CheckLLMHealth(std::stop_token stopToken = {}) const;
    healthOutput CheckFastHealth() const;
    healthOutput CheckExpertHealth() const;
    // Optional tier acquisition: null uses endpoint availability; installed coordinators reload cold tiers.
    // The session owns the coordinator and must outlive its router reference.
    void SetLifetimeCoordinator(revia::intelligence::ModelLifetimeCoordinator* coordinator);

    // The inventory the router keeps for its own tiers, so the session that owns the
    // processes can hand it to a lifetime coordinator instead of keeping a second one.
    [[nodiscard]] revia::intelligence::ModelResidencyManager& Residency() const
    { return residency; }

    void SetTierResidency(revia::intelligence::IntelligenceTier tier,
        bool available, bool warm, double loadMilliseconds, const std::string& detail = {});
    [[nodiscard]] std::vector<revia::intelligence::ModelResidency> ModelResidencySnapshot() const;
    healthOutput CheckEmbeddingHealth(std::stop_token stopToken = {}) const;
    [[nodiscard]] const std::string& EmbeddingModelName() const
    { return embeddingConfiguration.modelName; }
    embeddingOutput EmbedMemory(const std::string& summary, std::stop_token stopToken = {}) const;
    void ApplyLLMSettings(const llmSettings& settings, const llmSettings& fastSettings, const llmSettings& expertSettings,
        const embeddingSettings& embeddingSettings, const aiProfile& profile, bool fastEnabled, bool expertEnabled);
    // Compatibility path for tests and profile reloads that intentionally configure
    // only the primary endpoint.
    void ApplyLLMSettings(const llmSettings& settings, const embeddingSettings& embeddingSettings, const aiProfile& profile);
    void ApplyProfile(const aiProfile& profile);
    bool IsExitCommand(const std::string &input) const;

private:
    [[nodiscard]] const std::string& ConfiguredModelName(const llmService& service) const;

    // True when the Fast brain is pinned to the CPU while Main has a GPU. Then Main is
    // quicker for everything, including the short turns Fast exists for. With no GPU
    // named for Main (automatic or CPU placement) the small model keeps its turns.
    [[nodiscard]] bool FastIsSlowerThanMain() const;

    llmService llm;
    llmService fastLlm;
    llmService expertLlm;
    bool fastConfigured = false;
    bool expertConfigured = false;
    revia::intelligence::ModelLifetimeCoordinator* lifetime = nullptr;
    llmSettings mainConfiguration;
    llmSettings fastConfiguration;
    llmSettings expertConfiguration;
    embeddingSettings embeddingConfiguration;
    mutable revia::intelligence::ModelResidencyManager residency;
    mutable std::mutex reasoningMutex;
    mutable ReasoningHandler pendingReasoning;
    mutable std::optional<revia::llm::cloud::CloudTurnRequest> pendingCloud;
    revia::llm::cloud::CloudChatService cloud;
};
