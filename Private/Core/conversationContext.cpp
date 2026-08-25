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
    TrimToBudget();
}

bool conversationContext::RemoveLastMessageIf(
    const std::string& role,
    const std::string& content)
{
    if (messages.empty() || messages.back().role != role ||
        messages.back().content != content)
    {
        return false;
    }
    messages.pop_back();
    return true;
}

void conversationContext::Clear()
{
    messages.clear();
}

std::vector<conversationMessage> conversationContext::GetRecentMessages() const
{
    return messages;
}

std::size_t conversationContext::CharacterCount() const
{
    std::size_t total = 0;
    for (const conversationMessage& message : messages)
    {
        total += message.content.size();
    }
    return total;
}

void conversationContext::TrimToBudget()
{
    // Keep the newest message even when it alone exceeds the soft character budget. The
    // current request must never be removed in order to preserve older context.
    while (messages.size() > 1 &&
        (messages.size() > maxMessages || CharacterCount() > maxCharacters))
    {
        // Conversation normally alternates user/assistant. Evict a complete old exchange
        // when possible so the retained context never begins with an orphaned answer.
        const bool completePair = messages.size() >= 2 &&
            messages[0].role == "user" && messages[1].role == "assistant";
        messages.erase(messages.begin(), messages.begin() + (completePair ? 2 : 1));
    }
}
