#pragma once

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
};

[[nodiscard]] bool VerifyImageArtifact(const std::filesystem::path& expectedPath, const std::string& jobId, const std::string& model,
    int width, int height, const ImageArtifactReceipt& receipt, std::string& outError);
}
