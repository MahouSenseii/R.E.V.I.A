#pragma once

#include <string>
#include <vector>

#include "LLM/llmService.h"

class messageRouter
{
public:
    messageRouter();
    ~messageRouter();

    responseOutput RouteMessage(const std::string &message, const std::vector<conversationMessage> &context) const;
    bool IsLLMAvailable() const;
    healthOutput CheckLLMHealth() const;
    void ApplyLLMSettings(const llmSettings& settings, const aiProfile& profile);
    bool IsExitCommand(const std::string &input) const;

private:

    llmService llm;
};
