#pragma once

#include "Core/conversationMessage.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace revia::llm
{
inline constexpr std::size_t MaximumVideoFrameReplyBytes = 128U * 1024U;

struct VideoFrameReplyOptions
{
    std::vector<std::string> requestedRegions;
    bool includeLiteralText = false;
    bool includeVisibleFacts = true;
};

struct VideoFrameReplyResult
{
    // Confirms complete schema/source-metadata conformance, not the truth of model observations.
    bool ok = false;
    std::string text;
    std::string error;
};

[[nodiscard]] std::string BuildVideoFrameReplySchema(const std::vector<conversationImage>& frames,
    const std::vector<std::string>& requestedRegions, std::string& outError);
[[nodiscard]] VideoFrameReplyResult ParseVideoFrameReply(std::string_view reply, const std::vector<conversationImage>& frames,
    const VideoFrameReplyOptions& options);
}
