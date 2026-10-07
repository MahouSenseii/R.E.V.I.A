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
inline constexpr std::size_t MaximumReplySchemaBytes = 8192;
inline constexpr std::size_t MaximumReplySchemaKeys = 64;
inline constexpr std::size_t MaximumReplySchemaDepth = 8;

enum class ReplyContractStatus
{
    Conversation,
    TypeOnly,
    ExplicitShape,
    Unsupported
};

struct ReplyContract
{
    ReplyFormat rootKind = ReplyFormat::Conversation;
    ReplyContractStatus extractionStatus = ReplyContractStatus::Conversation;
    std::string schemaJson;
    std::string reason;
};

[[nodiscard]] ReplyFormat RequestedReplyFormat(const std::string& input);
[[nodiscard]] ReplyContract RequestedReplyContract(const std::string& input);
[[nodiscard]] bool MatchesReplyContract(const std::string& text, const ReplyContract& contract);
[[nodiscard]] bool IsCompleteJsonReply(const std::string& text, std::size_t maxBytes = MaximumJsonReplyBytes);
}
