#include "Core/messageRouter.h"
#include "Core/runtimePath.h"

#include <algorithm>
#include <cctype>

namespace
{
std::uint64_t ArtifactMiB(
    const std::string& modelPath,
    const std::string& projectorPath = {})
{
    std::error_code error;
    std::uintmax_t bytes = std::filesystem::file_size(
        revia::core::ResolveRuntimePath(modelPath), error);
    if (error) bytes = 0;
    if (!projectorPath.empty())
    {
        error.clear();
        const std::uintmax_t projector = std::filesystem::file_size(
            revia::core::ResolveRuntimePath(projectorPath), error);
        if (!error) bytes += projector;
    }
    return static_cast<std::uint64_t>((bytes + 1024 * 1024 - 1) / (1024 * 1024));
}
}
messageRouter::messageRouter() = default;

messageRouter::~messageRouter() = default;

responseOutput messageRouter::RouteMessage(
    const std::string& message,
    const std::vector<conversationMessage>& context,
    const std::stop_token stopToken,
    DeltaHandler onDelta,
    const revia::intelligence::IntelligenceDecision& decision) const
{
    responseOutput output;

    if (message.empty())
    {
        output.bSuccess = false;
        output.response = "I didn't hear anything.";
        output.reason = "Input message was empty.";
        return output;
    }

    const llmService* selected = &llm;
    revia::intelligence::IntelligenceTier effectiveTier =
        revia::intelligence::IntelligenceTier::Main;
    std::string selectedTier = "Main";
    std::string fallbackReason;
    switch (decision.selectedTier)
    {
        case revia::intelligence::IntelligenceTier::Fast:
            if (fastConfigured && fastLlm.IsBackendAvailable())
            {
                selected = &fastLlm;
                effectiveTier = revia::intelligence::IntelligenceTier::Fast;
                selectedTier = "Fast";
            }
            else fallbackReason = "Fast endpoint unavailable; used Main.";
            break;
        case revia::intelligence::IntelligenceTier::Expert:
        case revia::intelligence::IntelligenceTier::ExpertVision:
            if (expertConfigured && expertLlm.IsBackendAvailable())
            {
                selected = &expertLlm;
                effectiveTier = revia::intelligence::IntelligenceTier::Expert;
                selectedTier = decision.selectedTier ==
                    revia::intelligence::IntelligenceTier::ExpertVision
                        ? "ExpertVision" : "Expert";
            }
            else fallbackReason = "Expert endpoint unavailable; used Main in Deep mode.";
            break;
        case revia::intelligence::IntelligenceTier::Vision:
            selectedTier = "Vision";
            break;
        default:
            break;
    }

    if (!selected->IsBackendAvailable() && selected != &fastLlm &&
        fastConfigured && fastLlm.IsBackendAvailable())
    {
        selected = &fastLlm;
        effectiveTier = revia::intelligence::IntelligenceTier::Fast;
        selectedTier = "Fast";
        fallbackReason = "Preferred endpoint unavailable; used the Fast brain.";
    }

    residency.BeginInference(effectiveTier, "interactive");
    DeltaHandler retryDelta = onDelta;
    // IntelligenceRouter owns the reasoning mode. In particular, a completed visible
    // self-inquiry changes the final-answer decision to Fast so the model does not spend
    // the same response allowance on a second, hidden deliberation.
    const bool deepReasoning =
        decision.mode == revia::intelligence::ReasoningMode::Deep;
    responseOutput routed = selected->GenerateResponse(
        context,
        stopToken,
        std::move(onDelta),
        deepReasoning);
    residency.EndInference(effectiveTier);
    const bool contextRejected = !routed.bSuccess &&
        routed.reason.find("HTTP status 400") != std::string::npos;
    if (contextRejected && selected != &llm && !stopToken.stop_requested() &&
        llm.IsBackendAvailable())
    {
        const std::string rejectedTier = selectedTier;
        residency.BeginInference(
            revia::intelligence::IntelligenceTier::Main, "fallback");
        routed = llm.GenerateResponse(
            context,
            stopToken,
            std::move(retryDelta),
            deepReasoning);
        residency.EndInference(revia::intelligence::IntelligenceTier::Main);
        selected = &llm;
        effectiveTier = revia::intelligence::IntelligenceTier::Main;
        selectedTier = "Main";
        fallbackReason = rejectedTier +
            " rejected the bounded request; used Main instead.";
    }
    routed.requestedTier = revia::intelligence::ToString(decision.requestedTier);
    routed.selectedTier = selectedTier;
    routed.selectedModel = selected == &fastLlm
        ? "Qwen3.5-0.8B-Q4_K_M.gguf"
        : selected == &expertLlm
            ? "Qwen3-VL-8B-Instruct-Unredacted-MAX.Q4_K_M.gguf"
            : "Qwen3.5-4B-Q4_K_M.gguf";
    routed.routingReason = decision.reason;
    routed.routingConfidence = decision.confidence;
    routed.bRoutingFallback = !fallbackReason.empty();
    routed.routingFallbackReason = std::move(fallbackReason);
    routed.reasoningMode = revia::intelligence::ToString(decision.mode);
    return routed;
}

void messageRouter::SetPosture(std::string posture)
{
    llm.SetPosture(posture);
    if (fastConfigured) fastLlm.SetPosture(posture);
    if (expertConfigured) expertLlm.SetPosture(std::move(posture));
}

responseOutput messageRouter::PlanAction(const std::string& request) const
{
    if (request.empty())
    {
        responseOutput output;
        output.bSuccess = false;
        output.response = "I need a task to plan.";
        output.reason = "Action planning request was empty.";
        output.bShouldSpeak = false;
        return output;
    }
    return llm.GenerateActionProposal(request);
}

responseOutput messageRouter::ReviewConversationReply(
    const std::string& userInput,
    const std::string& candidateReply,
    const std::string& runtimeGroundTruth,
    const int maxReviewTokens,
    const std::stop_token stopToken) const
{
    return llm.ReviewConversationReply(
        userInput, candidateReply, runtimeGroundTruth, maxReviewTokens, stopToken);
}

responseOutput messageRouter::GenerateCuriosityPlan(
    const std::string& boundedContextPrompt,
    const std::stop_token stopToken) const
{
    if (boundedContextPrompt.empty())
    {
        responseOutput output;
        output.reason = "Curiosity planning context was empty.";
        return output;
    }
    if (fastConfigured && fastLlm.IsBackendAvailable())
    {
        residency.BeginInference(
            revia::intelligence::IntelligenceTier::Fast, "background");
        responseOutput output = fastLlm.GenerateCuriosityPlan(
            boundedContextPrompt, stopToken);
        residency.EndInference(revia::intelligence::IntelligenceTier::Fast);
        output.requestedTier = "Fast";
        output.selectedTier = "Fast";
        output.selectedModel = "Qwen3.5-0.8B-Q4_K_M.gguf";
        output.routingReason = "Bounded curiosity nomination is a Fast background task.";
        return output;
    }
    responseOutput output = llm.GenerateCuriosityPlan(boundedContextPrompt, stopToken);
    output.requestedTier = "Fast";
    output.selectedTier = "Main";
    output.selectedModel = "Qwen3.5-4B-Q4_K_M.gguf";
    output.bRoutingFallback = true;
    output.routingFallbackReason = "Fast background brain was unavailable.";
    return output;
}

responseOutput messageRouter::Deliberate(
    const std::string& boundedInquiryPrompt,
    const std::stop_token stopToken) const
{
    if (boundedInquiryPrompt.empty())
    {
        responseOutput output;
        output.reason = "The self-inquiry envelope was empty.";
        return output;
    }
    // Main, not Expert, even though the Expert brain is what makes a turn qualify for an
    // inquiry. Expert is about to generate the answer this inquiry exists to improve, and
    // queueing both on it would double the wait on exactly the turns that are already the
    // slowest. The state packet travels in the envelope, so she is the same Revia either
    // way -- only the effort spent on the questions differs.
    if (llm.IsBackendAvailable())
    {
        residency.BeginInference(
            revia::intelligence::IntelligenceTier::Main, "interactive");
        responseOutput output = llm.Deliberate(boundedInquiryPrompt, stopToken);
        residency.EndInference(revia::intelligence::IntelligenceTier::Main);
        output.requestedTier = "Main";
        output.selectedTier = "Main";
        output.selectedModel = "Qwen3.5-4B-Q4_K_M.gguf";
        output.routingReason = "One bounded self-inquiry runs on the balanced Main brain.";
        return output;
    }
    if (fastConfigured && fastLlm.IsBackendAvailable())
    {
        residency.BeginInference(
            revia::intelligence::IntelligenceTier::Fast, "interactive");
        responseOutput output = fastLlm.Deliberate(boundedInquiryPrompt, stopToken);
        residency.EndInference(revia::intelligence::IntelligenceTier::Fast);
        output.requestedTier = "Main";
        output.selectedTier = "Fast";
        output.selectedModel = "Qwen3.5-0.8B-Q4_K_M.gguf";
        output.bRoutingFallback = true;
        output.routingFallbackReason = "The Main brain was unavailable for self-inquiry.";
        return output;
    }
    responseOutput output;
    output.reason = "No local brain was available for self-inquiry.";
    return output;
}

responseOutput messageRouter::PlanGoal(const std::string& request) const
{
    if (request.empty())
    {
        responseOutput output;
        output.bSuccess = false;
        output.response = "I need a task to plan.";
        output.reason = "Goal planning request was empty.";
        output.bShouldSpeak = false;
        return output;
    }
    return llm.GenerateGoalPlan(request);
}

responseOutput messageRouter::DrawDiagram(const std::string& request) const
{
    if (request.empty())
    {
        responseOutput output;
        output.bSuccess = false;
        output.response = "I need to know what to draw.";
        output.reason = "Diagram request was empty.";
        output.bShouldSpeak = false;
        return output;
    }
    return llm.GenerateDiagram(request);
}

responseOutput messageRouter::ComposeContent(
    const std::string& request,
    const std::string& context) const
{
    return llm.ComposeContent(request, context);
}

responseOutput messageRouter::ReviseBlock(
    const std::string& instruction,
    const std::string& neighbourhood,
    const std::string& target) const
{
    return llm.ReviseBlock(instruction, neighbourhood, target);
}

responseOutput messageRouter::AnalyzeImage(
    const std::filesystem::path& imagePath,
    const std::string& prompt,
    const int maxResponseTokens,
    const std::stop_token stopToken) const
{
    std::string lowered = prompt;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    const bool expertRequested =
        lowered.find("blueprint") != std::string::npos ||
        lowered.find("architecture") != std::string::npos ||
        lowered.find("difficult") != std::string::npos ||
        lowered.find("expert") != std::string::npos;
    const bool expertAvailable = expertRequested && expertConfigured &&
        expertLlm.IsBackendAvailable();
    responseOutput output = (expertAvailable ? expertLlm : llm).AnalyzeImage(
        imagePath, prompt, maxResponseTokens, stopToken);
    output.requestedTier = expertRequested ? "ExpertVision" : "Vision";
    output.selectedTier = expertAvailable ? "ExpertVision" : "Vision";
    output.selectedModel = expertAvailable
        ? "Qwen3-VL-8B-Instruct-Unredacted-MAX.Q4_K_M.gguf"
        : "Qwen3.5-4B-Q4_K_M.gguf";
    output.reasoningMode = expertRequested ? "Deep" : "Fast";
    output.routingReason = expertRequested
        ? "The visual prompt contains difficult architecture or Blueprint signals."
        : "Normal desktop perception uses the Main model and matching projector.";
    if (expertRequested && !expertAvailable)
    {
        output.bRoutingFallback = true;
        output.routingFallbackReason =
            "Expert vision was unavailable; used normal Main vision.";
    }
    return output;
}

memoryDecision messageRouter::EvaluateMemory(
    const std::string& userMessage,
    const std::string& assistantMessage,
    const std::stop_token stopToken) const
{
    if (fastConfigured && fastLlm.IsBackendAvailable())
    {
        residency.BeginInference(
            revia::intelligence::IntelligenceTier::Fast, "background");
        memoryDecision decision = fastLlm.EvaluateMemory(
            userMessage, assistantMessage, stopToken);
        residency.EndInference(revia::intelligence::IntelligenceTier::Fast);
        return decision;
    }
    return llm.EvaluateMemory(userMessage, assistantMessage, stopToken);
}

bool messageRouter::IsLLMAvailable() const
{
    return llm.IsBackendAvailable();
}

bool messageRouter::WarmUpLLM(
    const std::stop_token stopToken,
    std::string& outError) const
{
    return llm.WarmUp(stopToken, outError);
}

bool messageRouter::WarmUpFast(
    const std::stop_token stopToken,
    std::string& outError) const
{
    if (!fastConfigured)
    {
        outError = "The Fast brain is not configured.";
        return false;
    }
    return fastLlm.WarmUp(stopToken, outError);
}

bool messageRouter::WarmUpExpert(
    const std::stop_token stopToken,
    std::string& outError) const
{
    if (!expertConfigured)
    {
        outError = "The Expert brain is not configured.";
        return false;
    }
    return expertLlm.WarmUp(stopToken, outError);
}

healthOutput messageRouter::CheckLLMHealth() const
{
    return llm.CheckBackendHealth();
}

healthOutput messageRouter::CheckFastHealth() const
{
    return fastConfigured ? fastLlm.CheckBackendHealth() : healthOutput{};
}

healthOutput messageRouter::CheckExpertHealth() const
{
    return expertConfigured ? expertLlm.CheckBackendHealth() : healthOutput{};
}

void messageRouter::SetTierResidency(
    const revia::intelligence::IntelligenceTier tier,
    const bool available,
    const bool warm,
    const double loadMilliseconds,
    const std::string& detail)
{
    if (available) residency.MarkReady(tier, loadMilliseconds, warm);
    else residency.MarkFailed(tier, detail.empty() ? "The model is unavailable." : detail);
}

std::vector<revia::intelligence::ModelResidency>
messageRouter::ModelResidencySnapshot() const
{
    return residency.Snapshot();
}

healthOutput messageRouter::CheckEmbeddingHealth() const
{
    return llm.CheckEmbeddingHealth();
}

embeddingOutput messageRouter::EmbedMemory(
    const std::string& summary,
    const std::stop_token stopToken) const
{
    return llm.EmbedMemory(summary, stopToken);
}

bool messageRouter::IsExitCommand(const std::string& input) const
{
    return input == "exit" || input == "quit" || input == "bye";
}

void messageRouter::ApplyLLMSettings(
    const llmSettings& settings,
    const llmSettings& fastSettings,
    const llmSettings& expertSettings,
    const embeddingSettings& embeddingSettings,
    const aiProfile& profile,
    const bool fastEnabled,
    const bool expertEnabled)
{
    mainConfiguration = settings;
    fastConfiguration = fastSettings;
    expertConfiguration = expertSettings;
    embeddingConfiguration = embeddingSettings;
    llm.ApplySettings(settings, embeddingSettings, profile);
    fastConfigured = fastEnabled;
    expertConfigured = expertEnabled;
    if (fastConfigured) fastLlm.ApplySettings(fastSettings, embeddingSettings, profile);
    if (expertConfigured) expertLlm.ApplySettings(expertSettings, embeddingSettings, profile);
    revia::intelligence::ModelResidency mainResidency;
    mainResidency.tier = revia::intelligence::IntelligenceTier::Main;
    mainResidency.role = "Main";
    mainResidency.model = settings.modelName;
    mainResidency.projector = settings.bVisionEnabled
        ? settings.multimodalProjectorPath : "";
    mainResidency.device = settings.device;
    mainResidency.artifactMiB = ArtifactMiB(
        settings.modelPath, mainResidency.projector);
    residency.Register(std::move(mainResidency));

    revia::intelligence::ModelResidency fastResidency;
    fastResidency.tier = revia::intelligence::IntelligenceTier::Fast;
    fastResidency.role = "Fast";
    fastResidency.model = fastSettings.modelName;
    fastResidency.device = fastSettings.device;
    fastResidency.artifactMiB = ArtifactMiB(fastSettings.modelPath);
    fastResidency.state = fastConfigured
        ? revia::intelligence::ResidencyState::Cold
        : revia::intelligence::ResidencyState::Disabled;
    residency.Register(std::move(fastResidency));

    revia::intelligence::ModelResidency expertResidency;
    expertResidency.tier = revia::intelligence::IntelligenceTier::Expert;
    expertResidency.role = "Expert";
    expertResidency.model = expertSettings.modelName;
    expertResidency.projector = expertSettings.bVisionEnabled
        ? expertSettings.multimodalProjectorPath : "";
    expertResidency.device = expertSettings.device;
    expertResidency.artifactMiB = ArtifactMiB(
        expertSettings.modelPath, expertResidency.projector);
    expertResidency.state = expertConfigured
        ? revia::intelligence::ResidencyState::Cold
        : revia::intelligence::ResidencyState::Disabled;
    residency.Register(std::move(expertResidency));
}

void messageRouter::ApplyProfile(const aiProfile& profile)
{
    ApplyLLMSettings(
        mainConfiguration,
        fastConfiguration,
        expertConfiguration,
        embeddingConfiguration,
        profile,
        fastConfigured,
        expertConfigured);
}

void messageRouter::ApplyLLMSettings(
    const llmSettings& settings,
    const embeddingSettings& embeddingSettings,
    const aiProfile& profile)
{
    ApplyLLMSettings(settings, {}, {}, embeddingSettings, profile, false, false);
}
