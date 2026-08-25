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
    compressedHistory.clear();
}

std::vector<conversationMessage> conversationContext::GetRecentMessages() const
{
    return messages;
}

std::string conversationContext::GetCompressedHistorySummary() const
{
    if (compressedHistory.empty()) return {};
    return "Lossy summary of older dialogue (use only for continuity; recent turns and "
        "retrieved durable memories outrank it):\n" + compressedHistory;
}

void conversationContext::CompressOldMessage(const conversationMessage& message)
{
    constexpr std::size_t MaximumMessageCharacters = 280;
    std::string bounded = message.content;
    for (char& character : bounded)
    {
        if (character == '\r' || character == '\n') character = ' ';
    }
    if (bounded.size() > MaximumMessageCharacters)
    {
        bounded.resize(MaximumMessageCharacters);
        bounded += "...";
    }
    compressedHistory += message.role == "assistant" ? "Revia: " : "User: ";
    compressedHistory += bounded + '\n';
    if (compressedHistory.size() > maxSummaryCharacters)
    {
        const std::size_t excess = compressedHistory.size() - maxSummaryCharacters;
        const std::size_t nextLine = compressedHistory.find('\n', excess);
        compressedHistory.erase(
            0, nextLine == std::string::npos ? excess : nextLine + 1);
    }
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
        CompressOldMessage(messages[0]);
        if (completePair) CompressOldMessage(messages[1]);
        messages.erase(messages.begin(), messages.begin() + (completePair ? 2 : 1));
    }
}
