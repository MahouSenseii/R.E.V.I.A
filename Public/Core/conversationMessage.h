#pragma once

#include "Memory/memoryScope.h"
#include <optional>
#include <string>

struct conversationMessage
{
    std::string role;
    std::string content;
    std::optional<std::string> participantId{};
    std::optional<revia::memory::MemoryScope> memoryScope{};
};
