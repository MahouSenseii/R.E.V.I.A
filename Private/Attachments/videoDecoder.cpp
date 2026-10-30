#include "Attachments/videoDecoder.h"

#include "Actions/actionTypes.h"
#include "Core/runtimePath.h"
#include "Process/ownedProcess.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::attachments
{
namespace
{
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

struct ProcessOutput
{
    bool ok = false;
    bool cancelled = false;
    std::string bytes;
    std::string error;
};

bool LocalFile(const std::filesystem::path& path)
{
    if (!path.is_absolute() || path.empty())
        return false;
#ifdef _WIN32
    const auto native = path.native();
    if (native.starts_with(L"\\\\") || GetDriveTypeW(path.root_path().c_str()) == DRIVE_REMOTE)
        return false;
#endif
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

bool ValidLimits(const VideoLimits& limits)
{
    return limits.maxBytes > 0 && limits.maxBytes <= 64U * 1024U * 1024U && limits.maxDurationMs > 0 && limits.maxDurationMs <= 60000 &&
           limits.maxSide > 0 && limits.maxSide <= 1920 && limits.maxPixels > 0 && limits.maxPixels <= 2073600 && limits.maxFrames > 0 &&
           limits.maxFrames <= 4 && limits.frameSide > 0 && limits.frameSide <= 768 && limits.processTimeoutMs > 0 &&
           limits.processTimeoutMs <= 30000;
}

std::string ContainerMime(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::string header(4096, '\0');
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    header.resize(static_cast<std::size_t>(input.gcount()));
    if (header.size() >= 12 && header.substr(4, 4) == "ftyp")
    {
        const auto brand = header.substr(8, 4);
        if (brand == "isom" || brand == "iso2" || brand == "iso3" || brand == "iso4" || brand == "iso5" || brand == "iso6" ||
            brand == "mp41" || brand == "mp42" || brand == "avc1" || brand == "M4V ")
            return "video/mp4";
    }
    if (header.starts_with(std::string("\x1a\x45\xdf\xa3", 4)) && header.find(std::string("\x42\x82\x84", 3) + "webm") != std::string::npos)
        return "video/webm";
    return {};
}

std::string SourceError(const std::filesystem::path& path, const VideoLimits& limits)
{
    if (!ValidLimits(limits))
        return "The video decode limits are invalid.";
    if (!LocalFile(path))
        return "Video decoding requires an existing absolute local file; network media and directories are refused.";
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (error || bytes == 0 || bytes > limits.maxBytes)
        return "The video is empty, unreadable, or above the configured byte limit.";
    if (ContainerMime(path).empty())
        return "Only MP4/H.264 and WebM/VP8 or VP9 containers are supported; this file does not have a supported container header.";
    return {};
}

std::vector<std::string> InputArguments(const std::filesystem::path& path, const bool probe)
{
    std::vector<std::string> arguments{"-hide_banner", "-v", "error", "-max_alloc", "33554432", "-protocol_whitelist", "file",
        "-format_whitelist", "mov,matroska,webm", "-probesize", "8388608", "-analyzeduration", "5000000", "-threads", "1"};
    if (!probe)
        arguments.insert(
            arguments.end(), {"-nostdin", "-xerror", "-err_detect", "explode", "-filter_threads", "1", "-filter_complex_threads", "1"});
    if (ContainerMime(path) == "video/mp4")
        arguments.insert(arguments.end(), {"-enable_drefs", "0", "-use_absolute_path", "0"});
    return arguments;
}

ProcessOutput RunBounded(const std::filesystem::path& executable, const std::vector<std::string>& arguments, const std::size_t maxBytes,
    const Clock::time_point deadline, const std::stop_token stopToken)
{
    ProcessOutput result;
    if (stopToken.stop_requested())
    {
        result.cancelled = true;
        result.error = "Video decoding was cancelled.";
        return result;
    }
    process::OwnedProcess child;
    process::OwnedProcessRequest request;
    request.executable = executable;
    request.arguments = arguments;
    request.maxPendingOutputBytes = maxBytes;
    request.maxErrorBytes = 8192;
    if (!child.Start(request, result.error))
        return result;
    for (;;)
    {
        if (stopToken.stop_requested())
        {
            result.cancelled = true;
            result.error = "Video decoding was cancelled.";
            break;
        }
        if (Clock::now() >= deadline)
        {
            result.error = "The local video decoder exceeded its time limit.";
            break;
        }
        auto bytes = child.Read(std::chrono::milliseconds(10));
        if (bytes.size() > maxBytes - result.bytes.size())
        {
            result.error = "The local video decoder exceeded its output limit.";
            break;
        }
        result.bytes += bytes;
        if (const auto code = child.ExitCode(); code && !child.Running())
        {
            result.ok = *code == 0 && child.StopReason().empty();
            if (!result.ok)
                result.error = "The local video decoder failed or returned incomplete output.";
            break;
        }
        if (!child.StopReason().empty())
        {
            result.error = "The local video decoder exceeded its output limit.";
            break;
        }
    }
    child.Terminate();
    if (!result.ok)
        result.bytes.clear();
    return result;
}

std::optional<double> Number(const Json& value)
{
    try
    {
        if (value.is_number())
        {
            const auto number = value.get<double>();
            return std::isfinite(number) ? std::optional(number) : std::nullopt;
        }
        if (!value.is_string())
            return std::nullopt;
        const auto text = value.get<std::string>();
        std::size_t used = 0;
        const auto number = std::stod(text, &used);
        if (used == text.size() && std::isfinite(number))
            return number;
    }
    catch (const std::exception&)
    {
    }
    return std::nullopt;
}

std::optional<double> FrameRate(const Json& stream)
{
    for (const char* field : {"avg_frame_rate", "r_frame_rate"})
    {
        const auto text = stream.value(field, std::string());
        const auto slash = text.find('/');
        if (slash == std::string::npos)
            continue;
        const auto numerator = Number(text.substr(0, slash));
        const auto denominator = Number(text.substr(slash + 1));
        if (numerator && denominator && *numerator > 0 && *denominator > 0)
            return *numerator / *denominator;
    }
    return std::nullopt;
}

bool SameInfo(const VideoInfo& left, const VideoInfo& right)
{
    return left.mimeType == right.mimeType && left.videoCodec == right.videoCodec && left.audioCodec == right.audioCodec &&
           left.width == right.width && left.height == right.height && left.durationMs == right.durationMs &&
           left.hasAudio == right.hasAudio;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> PngDimensions(const std::string& png, const std::uint32_t maxSide)
{
    if (png.size() < 45 || !png.starts_with(std::string("\x89PNG\r\n\x1a\n", 8)) || png.substr(12, 4) != "IHDR" ||
        png.substr(png.size() - 8, 4) != "IEND")
        return std::nullopt;
    const auto integer = [&](const std::size_t offset)
    {
        std::uint32_t value = 0;
        for (std::size_t index = offset; index < offset + 4; ++index)
            value = (value << 8) | static_cast<unsigned char>(png[index]);
        return value;
    };
    const auto width = integer(16);
    const auto height = integer(20);
    if (width == 0 || height == 0 || width > maxSide || height > maxSide)
        return std::nullopt;
    return std::pair(width, height);
}

std::string Seconds(const std::int64_t milliseconds)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << milliseconds / 1000 << '.' << std::setfill('0') << std::setw(3) << milliseconds % 1000;
    return out.str();
}
}

VideoDecoder::VideoDecoder(VideoDecoderSettings settings) : settings(std::move(settings))
{
    const auto installedTool = [](const std::filesystem::path& relative)
    {
        const auto fromRuntime = core::FindInstalledArtifact(core::RuntimeRoot(), relative);
        return fromRuntime.empty() ? core::ResolveRuntimePath(relative) : fromRuntime;
    };
    if (this->settings.ffmpegPath.empty())
        this->settings.ffmpegPath = installedTool("ThirdParty/ffmpeg/bin/ffmpeg.exe");
    if (this->settings.ffprobePath.empty())
        this->settings.ffprobePath = installedTool("ThirdParty/ffmpeg/bin/ffprobe.exe");
}

std::string VideoDecoder::Availability() const
{
    if (!LocalFile(settings.ffmpegPath) || !LocalFile(settings.ffprobePath))
        return "Short-video support needs the installed local FFmpeg and ffprobe executables in ThirdParty/ffmpeg/bin.";
    return {};
}

std::string FormatVideoTimestamp(std::int64_t milliseconds)
{
    milliseconds = std::max<std::int64_t>(0, milliseconds);
    std::ostringstream out;
    out << std::setfill('0');
    if (milliseconds >= 3600000)
        out << std::setw(2) << milliseconds / 3600000 << ':';
    out << std::setw(2) << (milliseconds / 60000) % 60 << ':' << std::setw(2) << (milliseconds / 1000) % 60 << '.' << std::setw(3)
        << milliseconds % 1000;
    return out.str();
}

std::vector<std::int64_t> SampleVideoInterval(
    const std::int64_t durationMs, const std::int64_t startMs, const std::int64_t endMs, const std::size_t count)
{
    if (durationMs <= 0 || startMs < 0 || endMs < startMs || startMs >= durationMs || count == 0 || count > 4)
        return {};
    const auto end = std::min(endMs, durationMs - 1);
    std::vector<std::int64_t> result;
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto timestamp = count == 1 ? startMs : startMs + (end - startMs) * static_cast<std::int64_t>(index) / (count - 1);
        if (result.empty() || result.back() != timestamp)
            result.push_back(timestamp);
    }
    return result;
}

VideoProbeResult VideoDecoder::Probe(const std::filesystem::path& path, const VideoLimits& limits, const std::stop_token stopToken) const
{
    VideoProbeResult result;
    if (stopToken.stop_requested())
    {
        result.cancelled = true;
        result.error = "Video probing was cancelled.";
        return result;
    }
    result.error = Availability();
    if (result.error.empty())
        result.error = SourceError(path, limits);
    if (!result.error.empty())
        return result;
    auto arguments = InputArguments(path, true);
    arguments.insert(arguments.end(), {"-show_entries",
                                          "format=format_name,duration:stream=index,codec_type,codec_name,width,height,duration,avg_frame_"
                                          "rate,r_frame_rate,nb_frames:stream_disposition=attached_pic",
                                          "-of", "json", actions::PathToUtf8(path)});
    auto output =
        RunBounded(settings.ffprobePath, arguments, 65536, Clock::now() + std::chrono::milliseconds(limits.processTimeoutMs), stopToken);
    if (!output.ok)
    {
        result.cancelled = output.cancelled;
        result.error = std::move(output.error);
        return result;
    }
    try
    {
        const auto metadata = Json::parse(output.bytes);
        const auto& streams = metadata.at("streams");
        if (!streams.is_array() || streams.empty() || streams.size() > 2)
            throw std::runtime_error("The video must contain one video stream and at most one optional audio stream.");
        result.info.mimeType = ContainerMime(path);
        const auto container = metadata.at("format").value("format_name", std::string());
        if ((result.info.mimeType == "video/mp4" && container.find("mp4") == std::string::npos) ||
            (result.info.mimeType == "video/webm" && container.find("webm") == std::string::npos))
            throw std::runtime_error("The decoded container did not match the uploaded bytes.");
        std::optional<double> duration = Number(metadata.at("format").value("duration", Json()));
        std::size_t videoCount = 0;
        std::size_t audioCount = 0;
        for (const auto& stream : streams)
        {
            const auto type = stream.value("codec_type", std::string());
            const auto codec = stream.value("codec_name", std::string());
            if (type == "video")
            {
                ++videoCount;
                result.info.videoCodec = codec;
                result.info.width = stream.value("width", 0u);
                result.info.height = stream.value("height", 0u);
                const auto frameRate = FrameRate(stream);
                if (!frameRate || *frameRate < 1.0 || *frameRate > 120.0 || result.info.width == 0 || result.info.height == 0 ||
                    result.info.width > limits.maxSide || result.info.height > limits.maxSide ||
                    static_cast<std::uint64_t>(result.info.width) * result.info.height > limits.maxPixels ||
                    stream.value("disposition", Json::object()).value("attached_pic", 0) != 0)
                    throw std::runtime_error("Video dimensions, frame rate, or stream type exceed the supported decode limits.");
                if ((result.info.mimeType == "video/mp4" && codec != "h264") ||
                    (result.info.mimeType == "video/webm" && codec != "vp8" && codec != "vp9"))
                    throw std::runtime_error("Supported video codecs are H.264 in MP4 and VP8 or VP9 in WebM.");
                const auto frames = Number(stream.value("nb_frames", Json()));
                if (frames && (*frames <= 0 || *frames > 7200))
                    throw std::runtime_error("The video exceeds the decoded frame-count ceiling.");
            }
            else if (type == "audio")
            {
                ++audioCount;
                result.info.hasAudio = true;
                result.info.audioCodec = codec;
                if ((result.info.mimeType == "video/mp4" && codec != "aac") ||
                    (result.info.mimeType == "video/webm" && codec != "opus" && codec != "vorbis"))
                    throw std::runtime_error("Optional audio must be AAC in MP4, or Opus/Vorbis in WebM; it is not visual evidence.");
            }
            else
                throw std::runtime_error("Additional subtitle, attachment, data, or unknown streams are not supported for uploaded clips.");
            if (const auto streamDuration = Number(stream.value("duration", Json()));
                streamDuration && (!duration || *streamDuration > *duration))
                duration = streamDuration;
        }
        if (videoCount != 1 || audioCount > 1 || !duration || *duration <= 0 ||
            *duration > static_cast<double>(limits.maxDurationMs) / 1000.0)
            throw std::runtime_error("The video has unsupported streams or missing, invalid, or excessive duration.");
        result.info.durationMs = static_cast<std::int64_t>(std::ceil(*duration * 1000.0));
        result.ok = !stopToken.stop_requested();
        result.cancelled = stopToken.stop_requested();
        result.error = result.ok ? std::string() : "Video probing was cancelled.";
    }
    catch (const std::runtime_error& error)
    {
        result.error = error.what();
    }
    catch (const std::exception&)
    {
        result.error = "The local video probe returned malformed metadata.";
    }
    return result;
}

VideoSampleResult VideoDecoder::Sample(const std::filesystem::path& path, const VideoInfo& info,
    const std::vector<std::int64_t>& requestedMs, const VideoLimits& limits, const std::stop_token stopToken) const
{
    VideoSampleResult result;
    const auto deadline = Clock::now() + std::chrono::milliseconds(limits.processTimeoutMs);
    const auto probe = Probe(path, limits, stopToken);
    if (!probe.ok || !SameInfo(probe.info, info))
    {
        result.cancelled = probe.cancelled;
        result.error = probe.ok ? "The stored video's metadata changed before sampling." : probe.error;
        return result;
    }
    if (requestedMs.empty() || requestedMs.size() > limits.maxFrames || !std::is_sorted(requestedMs.begin(), requestedMs.end()) ||
        std::any_of(requestedMs.begin(), requestedMs.end(), [&](const auto time) { return time < 0 || time >= info.durationMs; }))
    {
        result.error = "Requested video timestamps must be ordered, inside the clip, and within the frame budget.";
        return result;
    }
    auto arguments = InputArguments(path, true);
    arguments.insert(arguments.end(), {"-select_streams", "v:0", "-show_frames", "-show_entries",
                                          "frame=best_effort_timestamp_time,width,height", "-of", "json", actions::PathToUtf8(path)});
    auto output = RunBounded(settings.ffprobePath, arguments, 1024U * 1024U, deadline, stopToken);
    if (!output.ok)
    {
        result.cancelled = output.cancelled;
        result.error = std::move(output.error);
        return result;
    }
    try
    {
        const auto frames = Json::parse(output.bytes).at("frames");
        if (!frames.is_array() || frames.empty() || frames.size() > 7200)
            throw std::runtime_error("The decoded frame count is empty or above the supported ceiling.");
        std::vector<std::int64_t> timestamps;
        std::optional<double> firstTime;
        for (const auto& frame : frames)
        {
            const auto time = Number(frame.value("best_effort_timestamp_time", Json()));
            const auto width = frame.value("width", 0u);
            const auto height = frame.value("height", 0u);
            if (!time || std::abs(*time) > 1000000000.0 || width == 0 || height == 0 || width > limits.maxSide || height > limits.maxSide ||
                static_cast<std::uint64_t>(width) * height > limits.maxPixels)
                throw std::runtime_error("A decoded frame has invalid timing or dimensions.");
            if (!firstTime)
                firstTime = *time;
            const auto milliseconds = static_cast<std::int64_t>(std::llround((*time - *firstTime) * 1000.0));
            if (milliseconds < 0 || milliseconds >= info.durationMs || (!timestamps.empty() && milliseconds <= timestamps.back()))
                throw std::runtime_error("Decoded frame timestamps are missing, unordered, or outside the clip.");
            timestamps.push_back(milliseconds);
        }
        std::vector<std::size_t> selected;
        for (const auto requested : requestedMs)
        {
            const auto found = std::lower_bound(timestamps.begin(), timestamps.end(), requested);
            const auto index = found == timestamps.end() ? timestamps.size() - 1 : static_cast<std::size_t>(found - timestamps.begin());
            if (selected.empty() || selected.back() != index)
                selected.push_back(index);
        }
        for (const auto index : selected)
        {
            arguments = InputArguments(path, false);
            arguments.insert(
                arguments.end(), {"-i", actions::PathToUtf8(path), "-map", "0:v:0", "-an", "-sn", "-dn", "-vf",
                                     "select=eq(n\\," + std::to_string(index) + "),scale='min(" + std::to_string(limits.frameSide) +
                                         ",iw)':'min(" + std::to_string(limits.frameSide) + ",ih)':force_original_aspect_ratio=decrease",
                                     "-frames:v", "1", "-vsync", "0", "-threads", "1", "-c:v", "png", "-f", "image2pipe", "pipe:1"});
            const auto maxBytes = static_cast<std::size_t>(limits.frameSide) * limits.frameSide * 4 + 65536;
            auto png = RunBounded(settings.ffmpegPath, arguments, maxBytes, deadline, stopToken);
            if (!png.ok)
            {
                result.cancelled = png.cancelled;
                throw std::runtime_error(png.error);
            }
            const auto dimensions = PngDimensions(png.bytes, limits.frameSide);
            if (!dimensions)
                throw std::runtime_error("The local decoder did not return one complete bounded PNG frame.");
            conversationImage image;
            image.name = "Video frame at " + FormatVideoTimestamp(timestamps[index]);
            image.mimeType = "image/png";
            image.bytes = std::make_shared<const std::string>(std::move(png.bytes));
            image.width = dimensions->first;
            image.height = dimensions->second;
            result.frames.push_back({timestamps[index], std::move(image)});
        }
        result.ok = !stopToken.stop_requested();
        result.cancelled = stopToken.stop_requested();
        if (!result.ok)
            throw std::runtime_error("Video sampling was cancelled.");
    }
    catch (const std::runtime_error& error)
    {
        result.error = error.what();
        result.frames.clear();
    }
    catch (const std::exception&)
    {
        result.error = "The local decoder returned malformed frame metadata.";
        result.frames.clear();
    }
    return result;
}

VideoAudioResult VideoDecoder::ExtractAudio(const std::filesystem::path& path, const VideoInfo& info, const std::int64_t startMs,
    const std::int64_t endMs, const std::stop_token stopToken) const
{
    VideoAudioResult result;
    const auto probe = Probe(path, {}, stopToken);
    if (!probe.ok || !SameInfo(probe.info, info))
    {
        result.cancelled = probe.cancelled;
        result.error = probe.ok ? "The stored video's metadata changed before audio extraction." : probe.error;
        return result;
    }
    if (!info.hasAudio)
    {
        result.error = "This clip has no audio stream; silent frames do not provide sound or transcript evidence.";
        return result;
    }
    if (startMs < 0 || endMs <= startMs || endMs > info.durationMs || endMs - startMs > 60000)
    {
        result.error = "The requested audio interval must be inside the clip and no longer than 60 seconds.";
        return result;
    }
    auto arguments = InputArguments(path, false);
    arguments.insert(
        arguments.end(), {"-ss", Seconds(startMs), "-i", actions::PathToUtf8(path), "-t", Seconds(endMs - startMs), "-map", "0:a:0", "-vn",
                             "-sn", "-dn", "-ac", "1", "-ar", "16000", "-threads", "1", "-c:a", "pcm_s16le", "-f", "s16le", "pipe:1"});
    const auto maxBytes = static_cast<std::size_t>(endMs - startMs) * 32;
    auto pcm = RunBounded(settings.ffmpegPath, arguments, maxBytes, Clock::now() + std::chrono::seconds(15), stopToken);
    if (!pcm.ok || pcm.bytes.empty() || pcm.bytes.size() % 2 != 0 || stopToken.stop_requested())
    {
        result.cancelled = pcm.cancelled || stopToken.stop_requested();
        result.error = !pcm.error.empty() ? std::move(pcm.error) : "The clip did not yield complete bounded PCM audio for that interval.";
        return result;
    }
    const auto integer = [&](const std::uint32_t value, const int bytes)
    {
        for (int index = 0; index < bytes; ++index)
            result.wave.push_back(static_cast<char>((value >> (index * 8)) & 0xff));
    };
    result.wave = "RIFF";
    integer(static_cast<std::uint32_t>(pcm.bytes.size() + 36), 4);
    result.wave += "WAVEfmt ";
    integer(16, 4);
    integer(1, 2);
    integer(1, 2);
    integer(16000, 4);
    integer(32000, 4);
    integer(2, 2);
    integer(16, 2);
    result.wave += "data";
    integer(static_cast<std::uint32_t>(pcm.bytes.size()), 4);
    result.wave += pcm.bytes;
    result.ok = true;
    return result;
}

VideoPreviewResult VideoDecoder::StreamPreview(const std::filesystem::path& path, const VideoInfo& info, const std::int64_t startMs,
    const PreviewCallback& callback, const std::stop_token stopToken) const
{
    VideoPreviewResult result;
    const auto probe = Probe(path, {}, stopToken);
    if (!probe.ok || !SameInfo(probe.info, info))
    {
        result.cancelled = probe.cancelled;
        result.error = probe.ok ? "The stored video's metadata changed before preview." : probe.error;
        return result;
    }
    if (!callback || startMs < 0 || startMs >= info.durationMs)
    {
        result.error = "The preview needs a callback and a timestamp inside the clip.";
        return result;
    }
    auto arguments = InputArguments(path, false);
    arguments.insert(
        arguments.end(), {"-re", "-ss", Seconds(startMs), "-i", actions::PathToUtf8(path), "-map", "0:v:0", "-an", "-sn", "-dn", "-vf",
                             "fps=15,scale=640:360:force_original_aspect_ratio=decrease,pad=640:360:(ow-iw)/2:(oh-ih)/2", "-threads", "1",
                             "-pix_fmt", "rgb24", "-c:v", "rawvideo", "-f", "rawvideo", "pipe:1"});
    process::OwnedProcess child;
    process::OwnedProcessRequest request;
    request.executable = settings.ffmpegPath;
    request.arguments = std::move(arguments);
    constexpr std::size_t FrameBytes = VideoPreviewWidth * VideoPreviewHeight * 3;
    // Low-rate inputs emit a burst of duplicated display frames. The child is paced,
    // and this fixed pipe bound absorbs that burst while the UI consumes its queue.
    request.maxPendingOutputBytes = FrameBytes * 24;
    if (!child.Start(request, result.error))
        return result;
    const auto deadline = Clock::now() + std::chrono::milliseconds(info.durationMs - startMs + 15000);
    const auto maximumFrames = static_cast<std::size_t>((info.durationMs - startMs) * VideoPreviewFramesPerSecond / 1000 + 2);
    std::string pending;
    while (Clock::now() < deadline)
    {
        if (stopToken.stop_requested())
        {
            result.cancelled = true;
            break;
        }
        pending += child.Read(std::chrono::milliseconds(10));
        while (pending.size() >= FrameBytes)
        {
            const auto timestamp = startMs + static_cast<std::int64_t>(result.frames * 1000 / VideoPreviewFramesPerSecond);
            if (result.frames >= maximumFrames || timestamp >= info.durationMs)
            {
                result.error = "The decoder produced preview frames outside the validated clip duration.";
                break;
            }
            auto frame = pending.substr(0, FrameBytes);
            pending.erase(0, FrameBytes);
            ++result.frames;
            if (stopToken.stop_requested() || !callback(timestamp, frame))
            {
                result.cancelled = true;
                break;
            }
        }
        if (result.cancelled || !result.error.empty())
            break;
        if (const auto code = child.ExitCode(); code && !child.Running())
        {
            result.ok = *code == 0 && child.StopReason().empty() && pending.empty() && result.frames > 0;
            if (!result.ok)
                result.error = "The local video preview failed or ended with an incomplete frame.";
            break;
        }
        if (!child.StopReason().empty())
        {
            result.error = "The preview exceeded its bounded frame queue.";
            break;
        }
    }
    child.Terminate();
    if (!result.ok && result.error.empty())
        result.error = result.cancelled ? "Video preview was cancelled." : "Video preview exceeded its time limit.";
    return result;
}

} // namespace revia::attachments
