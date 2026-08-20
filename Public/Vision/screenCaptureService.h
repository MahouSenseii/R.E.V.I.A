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
    int originX = 0;
    int originY = 0;
    // Captured from Windows, never supplied by the model. This pins a screen action to
    // the actual foreground process before capability policy sees it.
    std::string foregroundApplication;
    std::string foregroundWindowTitle;
    double elapsedMilliseconds = 0.0;
};

class ScreenCaptureService
{
public:
    [[nodiscard]] CaptureResult CaptureDesktop(
        const std::filesystem::path& outputDirectory) const;

    // Captures only the foreground window and preserves its screen-space origin so
    // vision regions can be matched to UI Automation bounds without guessing.
    [[nodiscard]] CaptureResult CaptureForegroundWindow(
        const std::filesystem::path& outputDirectory) const;
};

} // namespace revia::vision
