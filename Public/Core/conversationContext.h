#pragma once

#include "Library/structLibrary.h"

#include <string>
#include <vector>

class conversationContext
{
public:
    conversationContext();
    ~conversationContext();

    void AddMessage(const std::string& role, const std::string& content);
    // Rolls back only the exact newest message. Used when fresh user input cancels an
    // autonomous result during its final commit race; older dialogue is never searched
    // or removed by content.
    [[nodiscard]] bool RemoveLastMessageIf(
        const std::string& role,
        const std::string& content);
    void Clear();

    std::vector<conversationMessage> GetRecentMessages() const;
    [[nodiscard]] std::string GetCompressedHistorySummary() const;

private:
    [[nodiscard]] std::size_t CharacterCount() const;
    void TrimToBudget();
    void CompressOldMessage(const conversationMessage& message);

    std::vector<conversationMessage> messages;
    std::string compressedHistory;
    std::size_t maxMessages = 24;
    std::size_t maxCharacters = 14000;
    std::size_t maxSummaryCharacters = 2400;
};
