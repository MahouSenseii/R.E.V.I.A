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
    std::vector<conversationMessage> messages;
    int maxMessages = 10;
};