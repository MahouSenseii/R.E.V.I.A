#include "LLM/llmService.h"

llmService::llmService() = default;

llmService::~llmService() = default;

void llmService::ApplySettings(
    const llmSettings& settings,
    const embeddingSettings& embeddingSettings,
    const aiProfile& profile)
{
    if (settings.backend == "LLamaCpp")
    {
        backendType = llmBackendType::LLamaCpp;
        llamaCpp.ApplySettings(settings, embeddingSettings, profile);
        bIsReady = true;
        return;
    }

    if (settings.backend == "Placeholder")
    {
        backendType = llmBackendType::Placeholder;
        bIsReady = true;
        return;
    }

    backendType = llmBackendType::None;
    bIsReady = false;
}

void llmService::SetPosture(std::string posture)
{
    // Only the local backend assembles its own prompt; the others have nowhere to put it.
    llamaCpp.SetPosture(std::move(posture));
}

healthOutput llmService::CheckEmbeddingHealth() const
{
    if (backendType == llmBackendType::LLamaCpp)
    {
        return llamaCpp.CheckEmbeddingHealth();
    }

    healthOutput output;
    output.name = "Embeddings";
    output.status = systemStatus::Yellow;
    output.message = "Semantic memory requires the llama.cpp backend.";
    output.reason = "The active language backend does not expose the configured embedding service.";
    return output;
}

embeddingOutput llmService::EmbedMemory(
    const std::string& summary,
    const std::stop_token stopToken) const
{
    if (backendType == llmBackendType::LLamaCpp)
    {
        return llamaCpp.EmbedMemory(summary, stopToken);
    }

    embeddingOutput output;
    output.reason = "The active language backend does not support memory embeddings.";
    return output;
}

bool llmService::IsBackendAvailable() const
{
    switch (backendType)
    {
        case llmBackendType::LLamaCpp:
            return llamaCpp.IsServerAvailable();

        case llmBackendType::Placeholder:
            return true;

        case llmBackendType::None:
        default:
            return false;
    }
}

healthOutput llmService::CheckBackendHealth() const
{
    switch (backendType)
    {
        case llmBackendType::LLamaCpp:
            return llamaCpp.CheckHealth();

        case llmBackendType::Placeholder:
        {
            healthOutput output;
            output.bIsAvailable = true;
            output.status = systemStatus::Green;
            output.name = "Placeholder";
            output.message = "Placeholder backend is available.";
            return output;
        }

        case llmBackendType::None:
        default:
        {
            healthOutput output;
            output.bIsAvailable = false;
            output.status = systemStatus::Red;
            output.name = "None";
            output.message = "No LLM backend is enabled.";
            output.reason = "Backend type is None or unsupported.";
            return output;
        }
    }
}

responseOutput llmService::GenerateResponse(
    const std::vector<conversationMessage>& context,
    const std::stop_token stopToken,
    DeltaHandler onDelta) const
{
    if (!bIsReady)
    {
        responseOutput output;
        output.bSuccess = false;
        output.response = "My language system is not ready yet.";
        output.reason = "LLM service was not ready.";
        output.bShouldSpeak = true;
        output.bShouldRemember = false;

        return output;
    }

    switch (backendType)
    {
        case llmBackendType::Placeholder:
            return GeneratePlaceholderResponse(context);

        case llmBackendType::LLamaCpp:
        {
            const healthOutput health = llamaCpp.CheckHealth();
            if (!health.bIsAvailable)
            {
                responseOutput output;
                output.bSuccess = false;
                output.response = "My configured language model is not available yet.";
                output.reason = health.reason;
                output.bShouldSpeak = true;
                output.bShouldRemember = false;
                return output;
            }
            return llamaCpp.GenerateResponse(context, stopToken, std::move(onDelta));
        }

        case llmBackendType::None:
        default:
        {
            responseOutput output;
            output.bSuccess = false;
            output.response = "No language backend is enabled.";
            output.reason = "Unsupported or disabled LLM backend.";
            output.bShouldSpeak = true;
            output.bShouldRemember = false;

            return output;
        }
    }
}

responseOutput llmService::GenerateActionProposal(const std::string& userRequest) const
{
    if (!bIsReady)
    {
        responseOutput output;
        output.bSuccess = false;
        output.response = "My language system is not ready to plan an action.";
        output.reason = "LLM service was not ready.";
        output.bShouldSpeak = false;
        return output;
    }

    switch (backendType)
    {
        case llmBackendType::LLamaCpp:
        {
            const healthOutput health = llamaCpp.CheckHealth();
            if (!health.bIsAvailable)
            {
                responseOutput output;
                output.bSuccess = false;
                output.response = "My configured language model is not available for planning yet.";
                output.reason = health.reason;
                output.bShouldSpeak = false;
                return output;
            }
            return llamaCpp.GenerateActionProposal(userRequest);
        }
        case llmBackendType::Placeholder:
        {
            responseOutput output;
            output.bSuccess = false;
            output.response = "The placeholder backend cannot plan filesystem actions.";
            output.reason = "Action planning requires a structured-output LLM backend.";
            output.bShouldSpeak = false;
            return output;
        }
        case llmBackendType::None:
        default:
        {
            responseOutput output;
            output.bSuccess = false;
            output.response = "No language backend is enabled for action planning.";
            output.reason = "Unsupported or disabled LLM backend.";
            output.bShouldSpeak = false;
            return output;
        }
    }
}

responseOutput llmService::ReviewConversationReply(
    const std::string& userInput,
    const std::string& candidateReply,
    const std::string& runtimeGroundTruth,
    const int maxReviewTokens,
    const std::stop_token stopToken) const
{
    if (!bIsReady || backendType != llmBackendType::LLamaCpp)
    {
        responseOutput output;
        output.reason = "AI response review requires the active llama.cpp backend.";
        return output;
    }
    return llamaCpp.ReviewConversationReply(
        userInput, candidateReply, runtimeGroundTruth, maxReviewTokens, stopToken);
}

responseOutput llmService::GenerateCuriosityPlan(
    const std::string& boundedContextPrompt,
    const std::stop_token stopToken) const
{
    if (!bIsReady || backendType != llmBackendType::LLamaCpp)
    {
        responseOutput output;
        output.reason = "Curiosity planning requires the active llama.cpp backend.";
        return output;
    }
    const healthOutput health = llamaCpp.CheckHealth();
    if (!health.bIsAvailable)
    {
        responseOutput output;
        output.reason = health.reason.empty()
            ? "The local model is unavailable for curiosity planning."
            : health.reason;
        return output;
    }
    return llamaCpp.GenerateCuriosityPlan(boundedContextPrompt, stopToken);
}

responseOutput llmService::GenerateGoalPlan(const std::string& userRequest) const
{
    if (!bIsReady)
    {
        responseOutput output;
        output.bSuccess = false;
        output.response = "My language system is not ready to plan a goal.";
        output.reason = "LLM service was not ready.";
        output.bShouldSpeak = false;
        return output;
    }

    if (backendType != llmBackendType::LLamaCpp)
    {
        responseOutput output;
        output.bSuccess = false;
        output.response = "Goal planning requires the local llama.cpp backend.";
        output.reason = "Goal planning requires a structured-output LLM backend.";
        output.bShouldSpeak = false;
        return output;
    }

    const healthOutput health = llamaCpp.CheckHealth();
    if (!health.bIsAvailable)
    {
        responseOutput output;
        output.bSuccess = false;
        output.response = "My configured language model is not available for planning yet.";
        output.reason = health.reason;
        output.bShouldSpeak = false;
        return output;
    }
    return llamaCpp.GenerateGoalPlan(userRequest);
}

responseOutput llmService::GenerateDiagram(const std::string& userRequest) const
{
    if (!bIsReady)
    {
        responseOutput output;
        output.bSuccess = false;
        output.response = "My language system is not ready to draw yet.";
        output.reason = "LLM service was not ready.";
        output.bShouldSpeak = false;
        return output;
    }

    if (backendType != llmBackendType::LLamaCpp)
    {
        responseOutput output;
        output.bSuccess = false;
        output.response = "Drawing requires the local llama.cpp backend.";
        output.reason = "Diagram generation requires a structured-output LLM backend.";
        output.bShouldSpeak = false;
        return output;
    }

    const healthOutput health = llamaCpp.CheckHealth();
    if (!health.bIsAvailable)
    {
        responseOutput output;
        output.bSuccess = false;
        output.response = "My configured language model is not available for drawing yet.";
        output.reason = health.reason;
        output.bShouldSpeak = false;
        return output;
    }
    return llamaCpp.GenerateDiagram(userRequest);
}

namespace
{
responseOutput ContentUnavailable(const std::string& what)
{
    responseOutput output;
    output.bSuccess = false;
    output.response = "My language system is not ready to " + what + " yet.";
    output.reason = "LLM service was not ready.";
    output.bShouldSpeak = false;
    return output;
}
}

responseOutput llmService::ComposeContent(
    const std::string& request,
    const std::string& context) const
{
    if (!bIsReady || backendType != llmBackendType::LLamaCpp ||
        !llamaCpp.CheckHealth().bIsAvailable)
    {
        return ContentUnavailable("draft");
    }
    return llamaCpp.ComposeContent(request, context);
}

responseOutput llmService::ReviseBlock(
    const std::string& instruction,
    const std::string& neighbourhood,
    const std::string& target) const
{
    if (!bIsReady || backendType != llmBackendType::LLamaCpp ||
        !llamaCpp.CheckHealth().bIsAvailable)
    {
        return ContentUnavailable("revise");
    }
    return llamaCpp.ReviseBlock(instruction, neighbourhood, target);
}

responseOutput llmService::AnalyzeImage(
    const std::filesystem::path& imagePath,
    const std::string& prompt,
    const int maxResponseTokens,
    const std::stop_token stopToken) const
{
    if (!bIsReady || backendType != llmBackendType::LLamaCpp)
    {
        responseOutput output;
        output.response = "My local vision system is not ready.";
        output.reason = "Vision requires the llama.cpp backend.";
        return output;
    }
    const healthOutput health = llamaCpp.CheckHealth();
    if (!health.bIsAvailable)
    {
        responseOutput output;
        output.response = "My local vision model is not available yet.";
        output.reason = health.reason;
        return output;
    }
    return llamaCpp.AnalyzeImage(imagePath, prompt, maxResponseTokens, stopToken);
}

memoryDecision llmService::EvaluateMemory(
    const std::string& userMessage,
    const std::string& assistantMessage,
    const std::stop_token stopToken) const
{
    if (!bIsReady)
    {
        memoryDecision decision;
        decision.reason = "LLM service was not ready for memory evaluation.";
        return decision;
    }

    switch (backendType)
    {
        case llmBackendType::LLamaCpp:
            return llamaCpp.EvaluateMemory(userMessage, assistantMessage, stopToken);
        case llmBackendType::Placeholder:
        {
            memoryDecision decision;
            decision.bSuccess = true;
            decision.reason = "The placeholder backend does not select memories.";
            return decision;
        }
        case llmBackendType::None:
        default:
        {
            memoryDecision decision;
            decision.reason = "No language backend is enabled for memory evaluation.";
            return decision;
        }
    }
}

responseOutput llmService::GeneratePlaceholderResponse(const std::vector<conversationMessage>& context) const
{
    responseOutput output;

    const std::string lastMessage = context.empty() ? "" : context.back().content;

    output.bSuccess = true;
    output.response = "LLM placeholder response to: " + lastMessage;
    output.reason = "";
    output.bShouldSpeak = true;
    output.bShouldRemember = false;

    return output;
}
