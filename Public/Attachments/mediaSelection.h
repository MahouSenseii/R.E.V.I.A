#pragma once

#include "Core/conversationMessage.h"

#include <string>
#include <vector>

namespace revia::attachments
{
struct MediaSelection
{
    std::vector<conversationMediaReference> references;
    std::string notice;
    bool requested = false;
};

[[nodiscard]] MediaSelection SelectConversationImages(
    const std::string& input, const std::vector<conversationMessage>& history, const memory::MemoryScope& scope);
[[nodiscard]] bool RequestsImageTransformation(const std::string& input);
// Current-request availability only; original upload totals and visual object counts stay separate.
[[nodiscard]] bool RequestsImageAttachmentCount(const std::string& input);
}
