#pragma once


#include "Core/conversationMessage.h"
#include <cstdint>
#include <string>
#include <vector>

class conversationContext
{
public:
    conversationContext();
    ~conversationContext();

    void AddMessage(const std::string& role, const std::string& content);
    void AddMessage(conversationMessage message);
    // Rolls back only the exact newest message. Used when fresh user input cancels an
    // autonomous result during its final commit race; older dialogue is never searched
    // or removed by content.
    [[nodiscard]] bool RemoveLastMessageIf(const std::string& role, const std::string& content);
    void Clear();
    // The caller admits archive scope before reconstruction; this owner never reads disk.
    void RestoreMessages(const std::vector<conversationMessage>& source, std::size_t recentMessages);

    std::vector<conversationMessage> GetRecentMessages() const;
    [[nodiscard]] std::string GetCompressedHistorySummary() const;

private:
    [[nodiscard]] std::size_t CharacterCount() const;
    [[nodiscard]] std::size_t MaximumRetainedMessageCharacters() const;
    void BoundRetainedMessage(conversationMessage& message) const;
    void TrimToBudget();
    struct ContinuityExcerpt
    {
        std::uint64_t source = 0;
        std::string role;
        std::string content;
        int priority = 0;
    };
    void CaptureContinuity(const conversationMessage& message, std::uint64_t source);

    std::vector<conversationMessage> messages;
    std::vector<std::uint64_t> messageSources;
    std::vector<ContinuityExcerpt> continuity;
    std::uint64_t nextSource = 1;
    std::size_t maxMessages = 24;
    std::size_t maxCharacters = 14000;
    std::size_t maxSummaryCharacters = 2400;
};
