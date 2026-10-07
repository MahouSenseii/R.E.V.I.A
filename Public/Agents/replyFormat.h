#pragma once

#include <cstddef>
#include <string>

namespace revia::agents
{
enum class ReplyFormat
{
    Conversation,
    JsonObject,
    JsonArray
};
inline constexpr std::size_t MaximumJsonReplyBytes = 262144;

[[nodiscard]] ReplyFormat RequestedReplyFormat(const std::string& input);
[[nodiscard]] bool IsCompleteJsonReply(const std::string& text, std::size_t maxBytes = MaximumJsonReplyBytes);
}
