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
    void Clear();

    std::vector<conversationMessage> GetRecentMessages() const;

private:
    [[nodiscard]] std::size_t CharacterCount() const;
    void TrimToBudget();

    std::vector<conversationMessage> messages;
    std::size_t maxMessages = 24;
    std::size_t maxCharacters = 14000;
};
