#pragma once

#include "Agents/responseProvenance.h"
#include <string>
#include <filesystem>
#include <stop_token>
#include <vector>

#include "LLM/llmService.h"
#include "LLM/privateMemoryAccess.h"
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
    // Set after SetPosture, which clears it: the lines this turn's answer must follow,
    // placed at the end of the prompt rather than in the system message.
    void SetReplyNote(std::string note);
    // The saved memories related to `query`, for work that happens before the reply's
    // own prompt is built -- the self-inquiry.
    std::string RelatedMemories(const std::string& query, std::stop_token stopToken = {}) const;
    responseOutput PlanAction(const std::string& request) const;
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
};
