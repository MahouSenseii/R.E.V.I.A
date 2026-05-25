#include "Core/conversationContext.h"

conversationContext::conversationContext() = default;

conversationContext::~conversationContext() = default;

void conversationContext::AddMessage(const std::string& role, const std::string& content)
{
    if (content.empty())
    {
        return;
    }

    messages.push_back({ role, content });

    while (messages.size() > static_cast<size_t>(maxMessages))
    {
        messages.erase(messages.begin());
    }
}

void conversationContext::Clear()
{
    messages.clear();
}

std::vector<conversationMessage> conversationContext::GetRecentMessages() const
{
    return messages;
}