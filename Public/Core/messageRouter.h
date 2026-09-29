#pragma once

#include "Agents/responseProvenance.h"
#include <string>
#include <filesystem>
#include <stop_token>
#include <vector>

#include "LLM/llmService.h"
#include "LLM/privateMemoryAccess.h"
#include "LLM/providerCapabilities.h"
#include "Intelligence/intelligenceTypes.h"
#include "Intelligence/modelLifetime.h"
#include "Intelligence/modelResidencyManager.h"

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
        DeltaHandler onDelta = {},
        const revia::intelligence::IntelligenceDecision& decision = {},
        revia::llm::PrivateMemoryAccess memoryAccess = revia::llm::PrivateMemoryAccess::ProfileSetting) const;
    void SetPosture(std::string posture);
    // Context that changes rarely -- the record of the earlier conversation -- placed
    // in the system message so llama.cpp reuses its cache between turns. Empty for a
    // turn that may not see private history.
    void SetStableContext(std::string context);
    // Set after SetPosture, which clears it: the lines this turn's answer must follow,
    // placed at the end of the prompt rather than in the system message.
    void SetReplyNote(std::string note);
    // The saved memories related to `query`, for work that happens before the reply's
    // own prompt is built -- the self-inquiry.
    std::string RelatedMemories(const std::string& query, std::stop_token stopToken = {}) const;
    // `toolCatalog`: the MCP tools the planner may name (Skills/mcpRegistry.h).
    responseOutput PlanAction(const std::string& request, const std::string& toolCatalog = {}) const;
    // A review of her own code. Expert when it can be had -- loaded on demand if it is
    // managed that way -- because this is the hardest reading she does and nobody is
    // waiting on it; Main otherwise.
    responseOutput ReviewCode(
        const std::string& instructions,
        const std::string& material,
        const std::string& schema,
        std::stop_token stopToken = {}) const;
    responseOutput ReviewConversationReply(
        const std::string& userInput,
        const std::string& candidateReply,
        const std::string& runtimeGroundTruth,
        int maxReviewTokens,
        std::stop_token stopToken = {}) const;
    // Returns one structured curiosity nomination. It never executes the nominated
    // research or decides whether Revia may interrupt.
    responseOutput GenerateActivityDraft(const std::string& topic,
        const std::string& context, std::stop_token stopToken = {}) const;
    responseOutput GenerateCuriosityPlan(
        const std::string& boundedContextPrompt,
        const std::vector<std::string>& availableActions,
        std::stop_token stopToken = {}) const;
    // Runs one bounded self-inquiry pass for a hard conversational turn. Interactive
    // priority, because the user's own reply is waiting behind it.
    responseOutput Deliberate(
        const std::string& boundedInquiryPrompt,
        std::stop_token stopToken = {}) const;
    // Records the oldest part of the conversation as observations, and merges an
    // overgrown record. Main only, at background priority: a person's turn preempts
    // either, and when Main is not there the history keeps its plain excerpts rather
    // than paying for the CPU model.
    responseOutput ObserveConversation(
        const std::string& boundedHistory,
        std::stop_token stopToken = {}) const;
    responseOutput ReflectOnConversation(
        const std::string& boundedRecord,
        std::stop_token stopToken = {}) const;
    // The quarantined reader over looked-up pages, at interactive priority: Fast when
    // it is local and there, because extraction under a schema is its size of job
    // and the reply is waiting; Main otherwise. Never a remote tier that may not
    // carry the question.
    responseOutput ReadWebPages(
        const std::string& boundedEnvelope,
        std::stop_token stopToken = {}) const;
    responseOutput PlanGoal(const std::string& request) const;
    // The iterative form: one step at a time, from what has already happened.
    // Ask Main for one bounded subgoal.
    //
    // A separate entry point from PlanNextGoalStep and not a convenience: the two ask
    // different questions under different grammars, and sharing one made the subgoal
    // question unanswerable.
    responseOutput PlanComputerSubgoal(
        const std::string& instruction,
        const std::string& situation,
        const std::string& schema,
        std::stop_token stopToken = {}) const;

    responseOutput PlanNextGoalStep(
        const std::string& goalContext,
        std::stop_token stopToken = {}) const;
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
    bool IsLLMAvailable() const;
    bool WarmUpLLM(std::stop_token stopToken, std::string& outError) const;
    bool WarmUpFast(std::stop_token stopToken, std::string& outError) const;
    bool WarmUpExpert(std::stop_token stopToken, std::string& outError) const;
    healthOutput CheckLLMHealth(std::stop_token stopToken = {}) const;
    healthOutput CheckFastHealth() const;
    healthOutput CheckExpertHealth() const;
    // Where "is this tier usable?" is answered when a tier may be put away.
    //
    // Optional and null by default, which is the behaviour that existed before: a tier
    // is usable when its endpoint answers, and nothing brings one back. With a
    // coordinator installed, a tier that is merely unloaded is acquired rather than
    // written off -- cold and unavailable being different things, and only the second
    // a reason to fall back.
    //
    // A raw pointer because the session owns the coordinator and outlives the router it
    // installs it on, exactly as it does for the residency inventory above.
    void SetLifetimeCoordinator(revia::intelligence::ModelLifetimeCoordinator* coordinator);

    // Whether a tier whose requests leave this machine may be handed private context.
    // Off, a remote Fast or Expert answers only turns that carry none, and memory
    // evaluation never leaves. Main is the owner's choice: if Main itself is remote,
    // that is the opt-in.
    void SetRemotePrivacy(bool allowPrivateContext);
    [[nodiscard]] bool TierIsRemote(revia::intelligence::IntelligenceTier tier) const;

    // The inventory the router keeps for its own tiers, so the session that owns the
    // processes can hand it to a lifetime coordinator instead of keeping a second one.
    [[nodiscard]] revia::intelligence::ModelResidencyManager& Residency() const
    { return residency; }

    void SetTierResidency(
        revia::intelligence::IntelligenceTier tier,
        bool available,
        bool warm,
        double loadMilliseconds,
        const std::string& detail = {});
    [[nodiscard]] std::vector<revia::intelligence::ModelResidency>
        ModelResidencySnapshot() const;
    healthOutput CheckEmbeddingHealth(std::stop_token stopToken = {}) const;
    [[nodiscard]] const std::string& EmbeddingModelName() const
    { return embeddingConfiguration.modelName; }
    embeddingOutput EmbedMemory(
        const std::string& summary,
        std::stop_token stopToken = {}) const;
    void ApplyLLMSettings(
        const llmSettings& settings,
        const llmSettings& fastSettings,
        const llmSettings& expertSettings,
        const embeddingSettings& embeddingSettings,
        const aiProfile& profile,
        bool fastEnabled,
        bool expertEnabled);
    // Compatibility path for tests and profile reloads that intentionally configure
    // only the primary endpoint.
    void ApplyLLMSettings(
        const llmSettings& settings,
        const embeddingSettings& embeddingSettings,
        const aiProfile& profile);
    void ApplyProfile(const aiProfile& profile);
    bool IsExitCommand(const std::string &input) const;

private:
    // True when the Fast brain is pinned to the CPU while Main has a GPU. Then Main is
    // quicker for everything, including the short turns Fast exists for. With no GPU
    // named for Main (automatic or CPU placement) the small model keeps its turns.
    [[nodiscard]] bool FastIsSlowerThanMain() const;
    // The settings a tier was configured with; Vision and ExpertVision map to the brain
    // that serves them.
    [[nodiscard]] const llmSettings& ConfigurationOf(
        revia::intelligence::IntelligenceTier tier) const;
    // The model name a service was configured with, which is what the Activity feed
    // shows. It used to be the file name of the shipped model, which was wrong the
    // moment settings named another.
    [[nodiscard]] const std::string& ModelNameOf(const llmService* service) const;
    // Whether a tier may be handed her private context: local, opted in, or Main is
    // itself remote and the owner has already sent everything there.
    [[nodiscard]] bool MayCarryPrivateContext(
        revia::intelligence::IntelligenceTier tier) const;
    [[nodiscard]] std::string RemoteNote(revia::intelligence::IntelligenceTier tier) const;

    llmService llm;
    llmService fastLlm;
    llmService expertLlm;
    bool fastConfigured = false;
    bool expertConfigured = false;
    bool allowRemotePrivateContext = false;
    revia::intelligence::ModelLifetimeCoordinator* lifetime = nullptr;
    llmSettings mainConfiguration;
    llmSettings fastConfiguration;
    llmSettings expertConfiguration;
    embeddingSettings embeddingConfiguration;
    mutable revia::intelligence::ModelResidencyManager residency;
};
