#include "LLM/promptBuilder.h"

promptBuilder::promptBuilder() = default;

promptBuilder::~promptBuilder() = default;

nlohmann::json promptBuilder::BuildMessages(const aiProfile& profile,
const std::vector<conversationMessage>& context) const
{
    nlohmann::json messages = nlohmann::json::array();

    std::string systemContent = profile.systemPrompt;

    const std::string memoryBlock = memory.BuildPromptBlock();

    if (!memoryBlock.empty())
    {
        if (!systemContent.empty())
        {
            systemContent += "\n\n";
        }

        systemContent += memoryBlock;
    }

    if (!systemContent.empty())
    {
        messages.push_back({
            {"role", "system"},
            {"content", systemContent}
        });
    }

    for (const conversationMessage& message : context)
    {
        if (message.content.empty())
        {
            continue;
        }

        messages.push_back({
            {"role", message.role},
            {"content", message.content}
        });
    }

    return messages;
}
