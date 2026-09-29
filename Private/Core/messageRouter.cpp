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
    const revia::intelligence::IntelligenceDecision& decision,
    const revia::llm::PrivateMemoryAccess memoryAccess) const
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
    // Declared here so it lives until the end of this call. A lease released at the end
    // of the switch would free the model while it is still generating.
    revia::intelligence::ModelLifetimeCoordinator::Lease expertLease;
    revia::intelligence::IntelligenceTier effectiveTier =
        revia::intelligence::IntelligenceTier::Main;
    std::string selectedTier = "Main";
    std::string fallbackReason;
    std::string routingNote;
    switch (decision.selectedTier)
    {
        case revia::intelligence::IntelligenceTier::Fast:
            if (FastIsSlowerThanMain() && llm.IsBackendAvailable())
            {
                // Small is not fast when small is on the CPU. Every reply carries the
                // system prompt and her state -- about 2,500 tokens -- and the CPU
                // brain reads those at ~270 tokens/s: "how are you" waited 8-10 s for
                // its first word, where Main on the GPU starts in about half a second.
                // The reasoning mode stays Fast, so the answer is still a brief one.
                routingNote = " The Fast brain runs on the CPU, so Main on the GPU "
                    "answered this short turn sooner.";
            }
            else if (fastConfigured &&
                !MayCarryPrivateContext(revia::intelligence::IntelligenceTier::Fast))
            {
                fallbackReason = "The Fast brain is a remote backend and is not given "
                    "her private context; used Main.";
            }
            else if (fastConfigured && fastLlm.IsBackendAvailable())
            {
                selected = &fastLlm;
                effectiveTier = revia::intelligence::IntelligenceTier::Fast;
                selectedTier = "Fast";
            }
            else fallbackReason = "Fast endpoint unavailable; used Main.";
            break;
        case revia::intelligence::IntelligenceTier::Expert:
        case revia::intelligence::IntelligenceTier::ExpertVision:
            // Cold is not unavailable. When a coordinator is managing this tier, being
            // unloaded is a reason to load it rather than a reason to give up on it --
            // and the lease it hands back is held for the whole generation below, so
            // the idle sweep cannot put the model away underneath the answer it is
            // producing. With no coordinator this is exactly the question it always
            // was, and the behaviour is unchanged.
            if (expertConfigured &&
                !MayCarryPrivateContext(revia::intelligence::IntelligenceTier::Expert))
            {
                fallbackReason = "The Expert brain is a remote backend and is not given "
                    "her private context; used Main in Deep mode.";
            }
            else if (expertConfigured)
            {
                const bool managed = lifetime != nullptr &&
                    lifetime->IsManaged(revia::intelligence::IntelligenceTier::Expert);
                if (managed)
                {
                    expertLease = lifetime->Acquire(
                        revia::intelligence::IntelligenceTier::Expert, stopToken);
                }
                // A managed tier is usable exactly when it was acquired, because
                // acquiring it is what brings it back. An unmanaged one is usable when
                // its endpoint answers, which is the only question this ever asked.
                const bool usable = managed
                    ? static_cast<bool>(expertLease) : expertLlm.IsBackendAvailable();
                if (usable)
                {
                    selected = &expertLlm;
                    effectiveTier = revia::intelligence::IntelligenceTier::Expert;
                    selectedTier = decision.selectedTier ==
                        revia::intelligence::IntelligenceTier::ExpertVision
                            ? "ExpertVision" : "Expert";
                }
                else fallbackReason =
                    "Expert endpoint unavailable; used Main in Deep mode.";
            }
            else fallbackReason = "Expert endpoint unavailable; used Main in Deep mode.";
            break;
        case revia::intelligence::IntelligenceTier::Vision:
            selectedTier = "Vision";
            break;
        default:
            break;
    }

    if (fastConfigured && selected != &fastLlm &&
        MayCarryPrivateContext(revia::intelligence::IntelligenceTier::Fast) &&
        !selected->IsBackendAvailable(stopToken) && fastLlm.IsBackendAvailable(stopToken))
    {
        selected = &fastLlm;
        effectiveTier = revia::intelligence::IntelligenceTier::Fast;
        selectedTier = "Fast";
        fallbackReason = "Preferred endpoint unavailable; used the Fast brain.";
    }
    routingNote += RemoteNote(effectiveTier);

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
        deepReasoning,
        memoryAccess);
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
            deepReasoning,
            memoryAccess);
        residency.EndInference(revia::intelligence::IntelligenceTier::Main);
        selected = &llm;
        effectiveTier = revia::intelligence::IntelligenceTier::Main;
        selectedTier = "Main";
        fallbackReason = rejectedTier +
            " rejected the bounded request; used Main instead.";
        routingNote = RemoteNote(effectiveTier);
    }
    routed.requestedTier = revia::intelligence::ToString(decision.requestedTier);
    routed.selectedTier = selectedTier;
    routed.selectedModel = ModelNameOf(selected);
    routed.routingReason = decision.reason + routingNote;
    routed.routingConfidence = decision.confidence;
    routed.bRoutingFallback = !fallbackReason.empty();
    routed.routingFallbackReason = std::move(fallbackReason);
    routed.reasoningMode = revia::intelligence::ToString(decision.mode);
    return routed;
}

responseOutput messageRouter::ReviewCode(
    const std::string& instructions,
    const std::string& material,
    const std::string& schema,
    const std::stop_token stopToken) const
{
    // Held for the whole review, so the idle sweep cannot unload Expert mid-answer.
    revia::intelligence::ModelLifetimeCoordinator::Lease expertLease;
    const llmService* selected = &llm;
    revia::intelligence::IntelligenceTier tier = revia::intelligence::IntelligenceTier::Main;
    bool loadedForThisReview = false;
    // The material is her own code or the owner's, so a remote Expert reads it only
    // with the same opt-in an interactive turn needs.
    if (expertConfigured &&
        MayCarryPrivateContext(revia::intelligence::IntelligenceTier::Expert))
    {
        const bool managed = lifetime != nullptr &&
            lifetime->IsManaged(revia::intelligence::IntelligenceTier::Expert);
        if (managed)
        {
            loadedForThisReview =
                !residency.IsResident(revia::intelligence::IntelligenceTier::Expert);
            expertLease = lifetime->Acquire(revia::intelligence::IntelligenceTier::Expert, stopToken);
        }
        if (managed ? static_cast<bool>(expertLease) : expertLlm.IsBackendAvailable(stopToken))
        {
            selected = &expertLlm;
            tier = revia::intelligence::IntelligenceTier::Expert;
        }
    }
    residency.BeginInference(tier, "background");
    responseOutput output = selected->GenerateCodeReview(instructions, material, schema, stopToken);
    residency.EndInference(tier);
    // Brought up only for this review, so put straight back. Left to the idle grace, it
    // kept the GPU 95% full for five more minutes, and everything else she does on her
    // own waited for it. One she was already using stays where it was.
    if (loadedForThisReview && expertLease)
    {
        expertLease.Release();
        (void)lifetime->ReleaseNow(revia::intelligence::IntelligenceTier::Expert);
    }
    output.selectedTier = revia::intelligence::ToString(tier);
    output.selectedModel = ModelNameOf(selected);
    output.routingReason += RemoteNote(tier);
    return output;
}

bool messageRouter::FastIsSlowerThanMain() const
{
    const auto onCpu = [](const std::string& device)
    {
        return device == "none" || device == "cpu" || device == "CPU";
    };
    return fastConfigured && onCpu(fastConfiguration.device) &&
        !mainConfiguration.device.empty() && !onCpu(mainConfiguration.device);
}

const llmSettings& messageRouter::ConfigurationOf(
    const revia::intelligence::IntelligenceTier tier) const
{
    switch (tier)
    {
        case revia::intelligence::IntelligenceTier::Fast:
            return fastConfiguration;
        case revia::intelligence::IntelligenceTier::Expert:
        case revia::intelligence::IntelligenceTier::ExpertVision:
            return expertConfiguration;
        default:
            return mainConfiguration;
    }
}

const std::string& messageRouter::ModelNameOf(const llmService* service) const
{
    if (service == &fastLlm) return fastConfiguration.modelName;
    if (service == &expertLlm) return expertConfiguration.modelName;
    return mainConfiguration.modelName;
}

bool messageRouter::TierIsRemote(const revia::intelligence::IntelligenceTier tier) const
{
    switch (tier)
    {
        case revia::intelligence::IntelligenceTier::Fast:
            return fastConfigured && revia::llm::IsCloudEndpoint(fastConfiguration);
        case revia::intelligence::IntelligenceTier::Expert:
        case revia::intelligence::IntelligenceTier::ExpertVision:
            return expertConfigured && revia::llm::IsCloudEndpoint(expertConfiguration);
        case revia::intelligence::IntelligenceTier::Reflex:
            return false;
        default:
            return revia::llm::IsCloudEndpoint(mainConfiguration);
    }
}

bool messageRouter::MayCarryPrivateContext(
    const revia::intelligence::IntelligenceTier tier) const
{
    return allowRemotePrivateContext || !TierIsRemote(tier) ||
        TierIsRemote(revia::intelligence::IntelligenceTier::Main);
}

std::string messageRouter::RemoteNote(const revia::intelligence::IntelligenceTier tier) const
{
    if (!TierIsRemote(tier)) return {};
    const llmSettings& configuration = ConfigurationOf(tier);
    return " Answered by a remote backend (" +
        std::string(revia::llm::CapabilitiesFor(configuration.backend).displayName) +
        " at " + configuration.host + ":" + std::to_string(configuration.port) + ").";
}

void messageRouter::SetRemotePrivacy(const bool allowPrivateContext)
{
    allowRemotePrivateContext = allowPrivateContext;
}

void messageRouter::SetPosture(std::string posture)
{
    llm.SetPosture(posture);
    if (fastConfigured) fastLlm.SetPosture(posture);
    if (expertConfigured) expertLlm.SetPosture(std::move(posture));
}

std::string messageRouter::RelatedMemories(
    const std::string& query,
    const std::stop_token stopToken) const
{
    return llm.RelatedMemories(query, stopToken);
}

void messageRouter::SetStableContext(std::string context)
{
    llm.SetStableContext(context);
    if (fastConfigured) fastLlm.SetStableContext(context);
    if (expertConfigured) expertLlm.SetStableContext(std::move(context));
}

void messageRouter::SetReplyNote(std::string note)
{
    llm.SetReplyNote(note);
    if (fastConfigured) fastLlm.SetReplyNote(note);
    if (expertConfigured) expertLlm.SetReplyNote(std::move(note));
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

responseOutput messageRouter::GenerateActivityDraft(
    const std::string& topic, const std::string& context, const std::stop_token stopToken) const
{
    residency.BeginInference(revia::intelligence::IntelligenceTier::Main, "background");
    auto result = llm.GenerateActivityDraft(topic, context, stopToken);
    residency.EndInference(revia::intelligence::IntelligenceTier::Main);
    return result;
}

responseOutput messageRouter::GenerateCuriosityPlan(
    const std::string& boundedContextPrompt,
    const std::vector<std::string>& availableActions,
    const std::stop_token stopToken) const
{
    if (boundedContextPrompt.empty())
    {
        responseOutput output;
        output.reason = "Curiosity planning context was empty.";
        return output;
    }
    // Topic selection needs more judgment than a cheap classifier. On the installed
    // hardware Main also completes this bounded job faster than the CPU Fast model.
    // It remains background inference, preemptible by an actual user turn.
    if (llm.IsBackendAvailable())
    {
        residency.BeginInference(revia::intelligence::IntelligenceTier::Main, "background");
        responseOutput output = llm.GenerateCuriosityPlan(boundedContextPrompt, availableActions, stopToken);
        residency.EndInference(revia::intelligence::IntelligenceTier::Main);
        output.requestedTier = output.selectedTier = "Main";
        output.selectedModel = mainConfiguration.modelName;
        output.routingReason = "Bounded self-directed topic selection uses the resident Main model.";
        return output;
    }
    if (fastConfigured && MayCarryPrivateContext(revia::intelligence::IntelligenceTier::Fast) &&
        fastLlm.IsBackendAvailable())
    {
        residency.BeginInference(
            revia::intelligence::IntelligenceTier::Fast, "background");
        responseOutput output = fastLlm.GenerateCuriosityPlan(
            boundedContextPrompt, availableActions, stopToken);
        residency.EndInference(revia::intelligence::IntelligenceTier::Fast);
        output.requestedTier = "Main";
        output.selectedTier = "Fast";
        output.selectedModel = fastConfiguration.modelName;
        output.bRoutingFallback = true;
        output.routingFallbackReason = "Main was unavailable; Fast can still nominate a bounded topic.";
        return output;
    }
    responseOutput output = llm.GenerateCuriosityPlan(boundedContextPrompt, availableActions, stopToken);
    output.requestedTier = "Main";
    output.selectedTier = "Main";
    output.selectedModel = mainConfiguration.modelName;
    output.routingReason = "No alternate background brain was available.";
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
        output.selectedModel = mainConfiguration.modelName;
        output.routingReason = "One bounded self-inquiry runs on the balanced Main brain.";
        return output;
    }
    if (fastConfigured && MayCarryPrivateContext(revia::intelligence::IntelligenceTier::Fast) &&
        fastLlm.IsBackendAvailable())
    {
        residency.BeginInference(
            revia::intelligence::IntelligenceTier::Fast, "interactive");
        responseOutput output = fastLlm.Deliberate(boundedInquiryPrompt, stopToken);
        residency.EndInference(revia::intelligence::IntelligenceTier::Fast);
        output.requestedTier = "Main";
        output.selectedTier = "Fast";
        output.selectedModel = fastConfiguration.modelName;
        output.bRoutingFallback = true;
        output.routingFallbackReason = "The Main brain was unavailable for self-inquiry.";
        return output;
    }
    responseOutput output;
    output.reason = "No local brain was available for self-inquiry.";
    return output;
}

namespace
{
// Main or nothing. The CPU-resident Fast model would hold nearly every core for the
// better part of a minute to do what the plain excerpts already do badly, and nobody
// is waiting on this: when Main is back, the next pass catches up.
template <class Call>
responseOutput OnMainInBackground(
    const llmService& llm,
    revia::intelligence::ModelResidencyManager& residency,
    const std::string& modelName,
    const std::string& input,
    const char* emptyReason,
    Call call)
{
    if (input.empty())
    {
        responseOutput output;
        output.reason = emptyReason;
        return output;
    }
    if (!llm.IsBackendAvailable())
    {
        responseOutput output;
        output.reason = "The Main brain is unavailable, so the history keeps its excerpts.";
        return output;
    }
    residency.BeginInference(revia::intelligence::IntelligenceTier::Main, "background");
    responseOutput output = call();
    residency.EndInference(revia::intelligence::IntelligenceTier::Main);
    output.requestedTier = "Main";
    output.selectedTier = "Main";
    output.selectedModel = modelName;
    output.routingReason = "The conversation record is kept on Main at background priority.";
    return output;
}
}

responseOutput messageRouter::ObserveConversation(
    const std::string& boundedHistory,
    const std::stop_token stopToken) const
{
    return OnMainInBackground(llm, residency, mainConfiguration.modelName, boundedHistory,
        "There was no conversation to record.",
        [&] { return llm.ObserveConversation(boundedHistory, stopToken); });
}

responseOutput messageRouter::ReflectOnConversation(
    const std::string& boundedRecord,
    const std::stop_token stopToken) const
{
    return OnMainInBackground(llm, residency, mainConfiguration.modelName, boundedRecord,
        "There was no record to reflect on.",
        [&] { return llm.ReflectOnConversation(boundedRecord, stopToken); });
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

responseOutput messageRouter::PlanComputerSubgoal(
    const std::string& instruction,
    const std::string& situation,
    const std::string& schema,
    const std::stop_token stopToken) const
{
    if (instruction.empty() || situation.empty())
    {
        responseOutput output;
        output.bSuccess = false;
        output.response = "I need to know what the task is before I can narrow it.";
        output.reason = "Computer subgoal context was empty.";
        output.bShouldSpeak = false;
        return output;
    }
    return llm.GenerateComputerSubgoal(instruction, situation, schema, stopToken);
}

responseOutput messageRouter::PlanNextGoalStep(
    const std::string& goalContext, const std::stop_token stopToken) const
{
    if (goalContext.empty())
    {
        responseOutput output;
        output.bSuccess = false;
        output.response = "I need the run so far to decide a next step.";
        output.reason = "Iterative goal context was empty.";
        output.bShouldSpeak = false;
        return output;
    }
    return llm.GenerateNextGoalStep(goalContext, stopToken);
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
    const std::stop_token stopToken,
    const bool backgroundAwareness) const
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
    // A screenshot is the most private thing she sees, so a remote Expert gets one
    // only with the opt-in.
    const bool expertAllowed = expertRequested && expertConfigured &&
        MayCarryPrivateContext(revia::intelligence::IntelligenceTier::ExpertVision);
    const bool expertAvailable = expertAllowed && expertLlm.IsBackendAvailable();
    responseOutput output = (expertAvailable ? expertLlm : llm).AnalyzeImage(
        imagePath, prompt, maxResponseTokens, stopToken, backgroundAwareness);
    output.requestedTier = expertRequested ? "ExpertVision" : "Vision";
    output.selectedTier = expertAvailable ? "ExpertVision" : "Vision";
    output.selectedModel = ModelNameOf(expertAvailable ? &expertLlm : &llm);
    output.reasoningMode = expertRequested ? "Deep" : "Fast";
    output.routingReason = (expertRequested
        ? "The visual prompt contains difficult architecture or Blueprint signals."
        : "Normal desktop perception uses the Main model and matching projector.") +
        RemoteNote(expertAvailable
            ? revia::intelligence::IntelligenceTier::ExpertVision
            : revia::intelligence::IntelligenceTier::Main);
    if (expertRequested && !expertAvailable)
    {
        output.bRoutingFallback = true;
        output.routingFallbackReason = expertConfigured && !expertAllowed
            ? "Expert vision is a remote backend and is not shown the screen; used "
              "normal Main vision."
            : "Expert vision was unavailable; used normal Main vision.";
    }
    return output;
}

memoryDecision messageRouter::EvaluateMemory(
    const std::string& userMessage,
    const std::string& assistantMessage,
    const revia::agents::ResponseProvenance provenance,
    const std::stop_token stopToken) const
{
    // Main first, like every other judgement that outlives the turn. The CPU-resident
    // 0.8B model took ~10 s of nearly every core per exchange, and what it decided was
    // wrong often enough to matter: replayed on a plain "I got back from the store, it
    // was raining" it saved an invented Revia preference copied from the prompt's own
    // example, and it filed the user's stated preference as Revia's. Those decisions
    // become durable memory that every later prompt carries. Main answers both correctly
    // in about a second of GPU time, at background priority, so a turn still preempts it.
    //
    // What is being judged is the exchange itself, which is the private context the
    // remote rule is about. Main is the owner's choice; a remote Fast stands in for it
    // only with the opt-in.
    if (llm.IsBackendAvailable())
    {
        residency.BeginInference(revia::intelligence::IntelligenceTier::Main, "background");
        memoryDecision decision =
            llm.EvaluateMemory(userMessage, assistantMessage, provenance, stopToken);
        residency.EndInference(revia::intelligence::IntelligenceTier::Main);
        return decision;
    }
    if (fastConfigured && MayCarryPrivateContext(revia::intelligence::IntelligenceTier::Fast) &&
        fastLlm.IsBackendAvailable())
    {
        residency.BeginInference(
            revia::intelligence::IntelligenceTier::Fast, "background");
        memoryDecision decision = fastLlm.EvaluateMemory(
            userMessage, assistantMessage, provenance, stopToken);
        residency.EndInference(revia::intelligence::IntelligenceTier::Fast);
        return decision;
    }
    if (fastConfigured && fastLlm.IsBackendAvailable())
    {
        memoryDecision decision;
        decision.reason = "Memory evaluation stays on this machine: Main is unavailable "
            "and the Fast brain is a remote backend that is not given her private context.";
        return decision;
    }
    return llm.EvaluateMemory(userMessage, assistantMessage, provenance, stopToken);
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

healthOutput messageRouter::CheckLLMHealth(const std::stop_token stopToken) const
{
    return llm.CheckBackendHealth(stopToken);
}

healthOutput messageRouter::CheckFastHealth() const
{
    return fastConfigured ? fastLlm.CheckBackendHealth() : healthOutput{};
}

healthOutput messageRouter::CheckExpertHealth() const
{
    return expertConfigured ? expertLlm.CheckBackendHealth() : healthOutput{};
}

void messageRouter::SetLifetimeCoordinator(
    revia::intelligence::ModelLifetimeCoordinator* coordinator)
{
    lifetime = coordinator;
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

healthOutput messageRouter::CheckEmbeddingHealth(std::stop_token stopToken) const
{
    return llm.CheckEmbeddingHealth(stopToken);
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
    // Profile changes do not reconfigure transports read by the memory worker.
    llm.ApplyProfile(mainConfiguration, profile);
    if (fastConfigured) fastLlm.ApplyProfile(fastConfiguration, profile);
    if (expertConfigured) expertLlm.ApplyProfile(expertConfiguration, profile);
}

void messageRouter::ApplyLLMSettings(
    const llmSettings& settings,
    const embeddingSettings& embeddingSettings,
    const aiProfile& profile)
{
    ApplyLLMSettings(settings, {}, {}, embeddingSettings, profile, false, false);
}
