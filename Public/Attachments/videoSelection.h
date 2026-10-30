#pragma once

#include "Attachments/mediaSelection.h"

#include <cstdint>

namespace revia::attachments
{
struct VideoInterval
{
    bool valid = true;
    bool explicitInterval = false;
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
    std::string error;
};

[[nodiscard]] bool IsVideoReference(const conversationMediaReference& reference);
[[nodiscard]] bool RequestsVideoTranscript(const std::string& input);
[[nodiscard]] bool RequestsOnlyVideoTranscript(const std::string& input);
[[nodiscard]] VideoInterval ParseVideoInterval(
    const std::string& input, std::int64_t durationMs, std::int64_t previousStartMs = 0, std::int64_t previousEndMs = -1);
[[nodiscard]] MediaSelection SelectConversationVideos(
    const std::string& input, const std::vector<conversationMessage>& history, const memory::MemoryScope& scope);
[[nodiscard]] MediaSelection SelectConversationMedia(
    const std::string& input, const std::vector<conversationMessage>& history, const memory::MemoryScope& scope);
}
