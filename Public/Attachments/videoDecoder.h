#pragma once

#include "Core/conversationMessage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::attachments
{

struct VideoDecoderSettings
{
    std::filesystem::path ffmpegPath;
    std::filesystem::path ffprobePath;
};

struct VideoLimits
{
    std::uintmax_t maxBytes = 64U * 1024U * 1024U;
    std::int64_t maxDurationMs = 60000;
    std::uint32_t maxSide = 1920;
    std::uint64_t maxPixels = 2073600;
    std::size_t maxFrames = 4;
    std::uint32_t frameSide = 768;
    int processTimeoutMs = 15000;
};

struct VideoInfo
{
    std::string mimeType;
    std::string videoCodec;
    std::string audioCodec;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int64_t durationMs = 0;
    bool hasAudio = false;
};

struct VideoProbeResult
{
    bool ok = false;
    bool cancelled = false;
    VideoInfo info;
    std::string error;
};

struct VideoFrame
{
    std::int64_t timestampMs = 0;
    conversationImage image;
};

struct VideoSampleResult
{
    bool ok = false;
    bool cancelled = false;
    std::vector<VideoFrame> frames;
    std::string error;
};

struct VideoPreviewResult
{
    bool ok = false;
    bool cancelled = false;
    std::size_t frames = 0;
    std::string error;
};

struct VideoAudioResult
{
    bool ok = false;
    bool cancelled = false;
    std::string wave;
    std::string error;
};

inline constexpr std::uint32_t VideoPreviewWidth = 640;
inline constexpr std::uint32_t VideoPreviewHeight = 360;
inline constexpr int VideoPreviewFramesPerSecond = 15;

[[nodiscard]] std::string FormatVideoTimestamp(std::int64_t milliseconds);
[[nodiscard]] std::vector<std::int64_t> SampleVideoInterval(
    std::int64_t durationMs, std::int64_t startMs, std::int64_t endMs, std::size_t count = 4);

// Only explicitly uploaded local files enter this decoder. It owns bounded child
// processes; the attachment library retains source identity, scope and lifetime.
class VideoDecoder
{
  public:
    using PreviewCallback = std::function<bool(std::int64_t timestampMs, const std::string& rgb24)>;

    explicit VideoDecoder(VideoDecoderSettings settings = {});
    // Empty when both configured local executables exist.
    [[nodiscard]] std::string Availability() const;
    [[nodiscard]] VideoProbeResult Probe(
        const std::filesystem::path& path, const VideoLimits& limits = {}, std::stop_token stopToken = {}) const;
    [[nodiscard]] VideoSampleResult Sample(const std::filesystem::path& path, const VideoInfo& info,
        const std::vector<std::int64_t>& requestedMs, const VideoLimits& limits = {}, std::stop_token stopToken = {}) const;
    // Canonical RIFF/WAV with bounded 16 kHz mono PCM. The caller owns any temporary file.
    [[nodiscard]] VideoAudioResult ExtractAudio(const std::filesystem::path& path, const VideoInfo& info, std::int64_t startMs,
        std::int64_t endMs, std::stop_token stopToken = {}) const;
    // RGB24 frames are letterboxed to 640 by 360 at 15 fps. The callback may block
    // to pace a bounded queue; returning false cancels this silent preview.
    [[nodiscard]] VideoPreviewResult StreamPreview(const std::filesystem::path& path, const VideoInfo& info, std::int64_t startMs,
        const PreviewCallback& callback, std::stop_token stopToken = {}) const;

  private:
    VideoDecoderSettings settings;
};

} // namespace revia::attachments
