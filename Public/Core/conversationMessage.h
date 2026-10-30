#pragma once

#include "Core/conversationMediaReference.h"
#include "Memory/memoryScope.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// A picture the user attached to one message. The bytes are shared because the turn's
// context is copied several times on its way to the model, and an image can be a few
// megabytes; they are only ever read.
struct conversationImage
{
    std::string name;
    std::string mimeType;
    std::shared_ptr<const std::string> bytes;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct conversationMessage
{
    std::string role;
    std::string content;
    std::optional<std::string> participantId{};
    std::optional<revia::memory::MemoryScope> memoryScope{};
    // Set only on the newest user message of a turn, never stored in history, so a
    // picture is sent with the message it came with and not again on every later turn.
    std::vector<conversationImage> images{};
    std::vector<conversationMediaReference> mediaReferences{};
};
