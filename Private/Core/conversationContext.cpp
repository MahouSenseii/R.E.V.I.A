#include "Core/utf8.h"
#include "Core/conversationContext.h"

#include <algorithm>
#include <string_view>

conversationContext::conversationContext() = default;

conversationContext::~conversationContext() = default;

void conversationContext::AddMessage(const std::string& role, const std::string& content)
{
    if (content.empty() || !revia::utf8::IsValid(content))
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
        revia::utf8::Truncate(bounded, MaximumMessageCharacters);
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
    for (std::size_t index = 0; index < messages.size(); ++index)
    {
        // The newest message counts at most its retained size. It is kept whole for the
        // turn that answers it, but a single enormous paste must not be the reason every
        // earlier exchange is evicted -- that emptied the whole conversation in a real
        // session, and the paste itself was bounded at the prompt layer anyway.
        const std::size_t size = messages[index].content.size();
        total += index + 1 == messages.size()
            ? std::min(size, MaximumRetainedMessageCharacters()) : size;
    }
    return total;
}

std::size_t conversationContext::MaximumRetainedMessageCharacters() const
{
    return std::max<std::size_t>(512, maxCharacters / 4);
}

void conversationContext::BoundRetainedMessage(conversationMessage& message) const
{
    const std::size_t limit = MaximumRetainedMessageCharacters();
    std::string& content = message.content;
    if (content.size() <= limit) return;
    static constexpr std::string_view marker = "\n[... middle of a long message omitted ...]\n";
    // Keep both ends: a pasted log's question is usually at the start and its error at
    // the end, and either alone reads as a different message.
    const std::size_t headBudget = (limit - marker.size()) * 2 / 3;
    const std::size_t tailBudget = limit - marker.size() - headBudget;
    std::size_t tailStart = content.size() - tailBudget;
    while (tailStart < content.size() &&
        (static_cast<unsigned char>(content[tailStart]) & 0xC0U) == 0x80U) ++tailStart;
    std::string bounded = revia::utf8::Prefix(content, headBudget);
    bounded += marker;
    bounded.append(content, tailStart, std::string::npos);
    content = std::move(bounded);
}

void conversationContext::TrimToBudget()
{
    // Everything but the current request is retained as a bounded excerpt.
    for (std::size_t index = 0; index + 1 < messages.size(); ++index)
    {
        BoundRetainedMessage(messages[index]);
    }
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
