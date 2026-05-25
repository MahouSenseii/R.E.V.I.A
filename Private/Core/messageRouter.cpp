#include "Core/messageRouter.h"
messageRouter::messageRouter() = default;

messageRouter::~messageRouter() = default;

responseOutput messageRouter::RouteMessage(const std::string& message,const std::vector<conversationMessage>& context)
const
{
    responseOutput output;

    if (message.empty())
    {
        output.bSuccess = false;
        output.response = "I didn't hear anything.";
        output.reason = "Input message was empty.";
        return output;
    }

    if (message == "hello" || message == "hi")
    {
        output.bSuccess = true;
        output.response = "Hi Quentin. I'm online.";
        return output;
    }

    if (message == "status")
    {
        output.bSuccess = true;
        output.response = "Core is running. MessageRouter and LLM service are active.";
        return output;
    }

    return llm.GenerateResponse(message, context);
}

bool messageRouter::IsLLMAvailable() const
{
    return llm.IsBackendAvailable();
}

healthOutput messageRouter::CheckLLMHealth() const
{
    return llm.CheckBackendHealth();
}

bool messageRouter::IsExitCommand(const std::string& input) const
{
    return input == "exit" || input == "quit" || input == "bye";
}

void messageRouter::ApplyLLMSettings(const llmSettings &settings, const aiProfile &profile)
{
    llm.ApplySettings(settings, profile);
}
