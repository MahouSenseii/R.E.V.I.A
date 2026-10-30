#pragma once

#include "Visual/imageRequest.h"

#include <cstdint>
#include <optional>
#include <string>

namespace revia::visual
{
enum class VideoRequestMode
{
    TextToVideo,
    ImageToVideo
};

struct VideoRequest
{
    VideoRequestMode mode = VideoRequestMode::TextToVideo;
    std::string prompt;
    std::optional<ImageSource> source;
    std::optional<std::uint32_t> seed;
};
}
