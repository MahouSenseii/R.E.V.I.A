#pragma once

#include "Visual/imageRequest.h"

#include <filesystem>
#include <string>

namespace revia::visual
{
struct ImageArtifactReceipt
{
    std::filesystem::path path;
    std::string sha256;
    std::string jobId;
    std::string model;
    int width = 0;
    int height = 0;
    ImageRequestMode mode = ImageRequestMode::TextToImage;
    std::string provider;
    std::string pipeline;
    std::string variant;
    std::uint32_t seed = 0;
    double requestedStrength = 0.0;
    double effectiveStrength = 0.0;
    int requestedSteps = 0;
    int totalInferenceSteps = 0;
    int denoisingSteps = 0;
    std::string sourceId;
    std::string sourceDigest;
    std::string preparedSourceDigest;
    std::string sourcePreparation;
};

struct ImageArtifactExpectation
{
    std::filesystem::path path;
    std::string jobId;
    std::string model;
    int width = 0;
    int height = 0;
    ImageRequestMode mode = ImageRequestMode::TextToImage;
    std::string provider = "local_diffusers";
    std::string pipeline;
    std::string variant;
    std::uint32_t seed = 0;
    double requestedStrength = 0.0;
    double effectiveStrength = 0.0;
    int requestedSteps = 0;
    int totalInferenceSteps = 0;
    int denoisingSteps = 0;
    std::string sourceId;
    std::string sourceDigest;
    std::string sourcePreparation;
};

[[nodiscard]] bool VerifyImageSource(const ImageSource& source, std::string& outError);
[[nodiscard]] bool VerifyImageArtifact(
    const ImageArtifactExpectation& expected, const ImageArtifactReceipt& receipt, std::string& outError);
[[nodiscard]] bool VerifyImageArtifact(const std::filesystem::path& expectedPath, const std::string& jobId, const std::string& model,
    int width, int height, const ImageArtifactReceipt& receipt, std::string& outError);
}
