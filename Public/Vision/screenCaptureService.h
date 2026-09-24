#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace revia::vision
{

struct MonitorDescriptor
{
    int index = 0;
    bool primary = false;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    std::string deviceName;

    [[nodiscard]] int Width() const { return right - left; }
    [[nodiscard]] int Height() const { return bottom - top; }
};

struct CaptureResult
{
    bool succeeded = false;
    std::filesystem::path path;
    std::string reason;
    int width = 0;
    int height = 0;
    int originX = 0;
    int originY = 0;
    int monitorCount = 0;
    int monitorIndex = 0;
    // Captured from Windows, never supplied by the model. This pins a screen action to
    // the actual foreground process before capability policy sees it.
    std::string foregroundApplication;
    std::string foregroundWindowTitle;
    double elapsedMilliseconds = 0.0;
};

class ScreenCaptureService
{
public:
    // Describes the current display topology without capturing pixels.
    [[nodiscard]] std::vector<MonitorDescriptor> EnumerateMonitors() const;

    [[nodiscard]] CaptureResult CaptureDesktop(
        const std::filesystem::path& outputDirectory) const;

    // Captures only the foreground window and preserves its screen-space origin so
    // vision regions can be matched to UI Automation bounds without guessing.
    [[nodiscard]] CaptureResult CaptureForegroundWindow(
        const std::filesystem::path& outputDirectory) const;
};

} // namespace revia::vision
