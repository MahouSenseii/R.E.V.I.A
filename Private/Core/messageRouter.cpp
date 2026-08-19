#include "Core/messageRouter.h"
messageRouter::messageRouter() = default;

messageRouter::~messageRouter() = default;

responseOutput messageRouter::RouteMessage(
    const std::string& message,
    const std::vector<conversationMessage>& context,
    const std::stop_token stopToken) const
{
    responseOutput output;

    if (message.empty())
    {
        output.bSuccess = false;
        output.response = "I didn't hear anything.";
        output.reason = "Input message was empty.";
        return output;
    }

    return llm.GenerateResponse(context, stopToken);
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

responseOutput messageRouter::AnalyzeImage(
    const std::filesystem::path& imagePath,
    const std::string& prompt,
    const int maxResponseTokens,
    const std::stop_token stopToken) const
{
    return llm.AnalyzeImage(imagePath, prompt, maxResponseTokens, stopToken);
}

memoryDecision messageRouter::EvaluateMemory(
    const std::string& userMessage,
    const std::stop_token stopToken) const
{
    return llm.EvaluateMemory(userMessage, stopToken);
}

bool messageRouter::IsLLMAvailable() const
{
    return llm.IsBackendAvailable();
}

healthOutput messageRouter::CheckLLMHealth() const
{
    return llm.CheckBackendHealth();
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
    const embeddingSettings& embeddingSettings,
    const aiProfile& profile)
{
    llm.ApplySettings(settings, embeddingSettings, profile);
}
