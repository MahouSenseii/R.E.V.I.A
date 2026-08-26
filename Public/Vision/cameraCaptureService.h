#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace revia::vision
{

// One video capture device, as Windows reports it.
//
// symbolicLink, not index, is the durable identity. Indices renumber when a device is
// unplugged or a virtual camera starts, so a setting that pinned "camera 1" would
// silently point at a different lens later. A stored preference should keep the link.
struct CameraDescriptor
{
    int index = 0;
    std::string name;
    std::string symbolicLink;
};

struct CameraFrame
{
    bool succeeded = false;
    std::filesystem::path path;
    std::string reason;
    int width = 0;
    int height = 0;
    std::string deviceName;
    // Frames discarded before the kept one. A webcam's first frames are black or badly
    // exposed while auto-gain settles, and describing those wastes a vision round trip
    // on an image of nothing.
    int warmupFramesDiscarded = 0;
    double elapsedMilliseconds = 0.0;
};

// Reads single frames from a local camera. Nothing more.
//
// Deliberately not a stream, and deliberately not resident. The service opens the
// device, takes what it was asked for, and closes it again, so the camera light is on
// only while a capture is actually happening. A companion that holds the webcam open
// for its whole session is indistinguishable from one that is recording, and the
// hardware light is the only signal a user actually trusts.
//
// Capture is not authority. Producing a frame here grants nothing: the capability gate
// decides whether this may be called at all, and the ordinary action path still governs
// anything done as a result of what the frame contained.
class CameraCaptureService
{
public:
    // Describes attached cameras without opening or reading from any of them, so a
    // settings screen can list devices without lighting up a lens.
    [[nodiscard]] std::vector<CameraDescriptor> EnumerateCameras() const;

    // Captures one frame as a PNG. cameraIndex is 1-based to match EnumerateCameras;
    // an empty symbolicLink means "whichever device holds that index right now".
    [[nodiscard]] CameraFrame CaptureFrame(
        const std::filesystem::path& outputDirectory,
        int cameraIndex = 1,
        const std::string& symbolicLink = {},
        int warmupFrames = 10) const;
};

} // namespace revia::vision
