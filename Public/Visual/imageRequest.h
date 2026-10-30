#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace revia::visual
{
enum class ImageRequestMode
{
    TextToImage,
    ReferenceToImage
};

struct ImageSource
{
    std::string id;
    std::string digest;
    std::string mimeType;
    std::shared_ptr<const std::string> bytes;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct ImageRequest
{
    ImageRequestMode mode = ImageRequestMode::TextToImage;
    std::string prompt;
    std::string negativePrompt;
    std::optional<ImageSource> source;
    double strength = 0.5;
    std::optional<std::uint32_t> seed;
};
}
