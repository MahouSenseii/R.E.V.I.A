#pragma once

#include <filesystem>
#include <string>

namespace revia::vision
{

struct CaptureResult
{
    bool succeeded = false;
    std::filesystem::path path;
    std::string reason;
    int width = 0;
    int height = 0;
    double elapsedMilliseconds = 0.0;
};

class ScreenCaptureService
{
public:
    [[nodiscard]] CaptureResult CaptureDesktop(
        const std::filesystem::path& outputDirectory) const;
};

} // namespace revia::vision
