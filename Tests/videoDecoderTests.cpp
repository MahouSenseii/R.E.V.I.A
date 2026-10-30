#include "Attachments/videoDecoder.h"
#include "Actions/actionTypes.h"
#include "Core/runtimePath.h"
#include "Process/ownedProcess.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <sys/wait.h>
#endif

namespace
{
using namespace revia::attachments;

void Check(const bool value, const std::string& message)
{
    if (!value)
        throw std::runtime_error(message);
}

struct Fixtures
{
    std::filesystem::path directory = std::filesystem::temp_directory_path() / ("revia-video-tests-" + revia::actions::NewActionId());
    Fixtures()
    {
        std::filesystem::create_directories(directory);
    }
    ~Fixtures()
    {
        std::error_code error;
        if (directory.parent_path() == std::filesystem::temp_directory_path() &&
            directory.filename().string().starts_with("revia-video-tests-"))
            std::filesystem::remove_all(directory, error);
    }
};

VideoDecoderSettings DecoderSettings()
{
    const auto installed = [](const std::filesystem::path& relative)
    {
        const auto found = revia::core::FindInstalledArtifact(revia::core::RuntimeRoot(), relative);
        return found.empty() ? revia::core::ResolveRuntimePath(relative) : found;
    };
    const char* ffmpeg = std::getenv("REVIA_TEST_FFMPEG");
    const char* ffprobe = std::getenv("REVIA_TEST_FFPROBE");
    return {ffmpeg ? revia::actions::Utf8ToPath(ffmpeg) : installed("ThirdParty/ffmpeg/bin/ffmpeg.exe"),
        ffprobe ? revia::actions::Utf8ToPath(ffprobe) : installed("ThirdParty/ffmpeg/bin/ffprobe.exe")};
}

std::string Run(const std::filesystem::path& executable, const std::vector<std::string>& arguments, const bool expectSuccess = true)
{
    revia::process::OwnedProcess process;
    revia::process::OwnedProcessRequest request;
    request.executable = executable;
    request.arguments = arguments;
    request.maxPendingOutputBytes = 4 * 1024 * 1024;
    std::string error;
    Check(process.Start(request, error), "Fixture process failed to start: " + error);
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while ((process.Running() || !process.ExitCode()) && std::chrono::steady_clock::now() < deadline)
        output += process.Read(std::chrono::milliseconds(10));
    Check(!process.Running() && process.ExitCode(), "Fixture process timed out.");
    const auto code = process.ExitCode();
    Check(code && ((*code == 0) == expectSuccess),
        "Fixture process exit disagreed: " + process.FinalErrorOutput(std::chrono::milliseconds(200)));
    return output;
}

void MakeClip(const VideoDecoderSettings& settings, const std::filesystem::path& target, const std::string& codec, const bool audio = false,
    const int frameRate = 4)
{
    const auto red = "color=c=red:s=160x96:r=" + std::to_string(frameRate) + ":d=1";
    const auto blue = "color=c=blue:s=160x96:r=" + std::to_string(frameRate) + ":d=1";
    std::vector<std::string> arguments{
        "-nostdin", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", red, "-f", "lavfi", "-i", blue};
    if (audio)
        arguments.insert(arguments.end(), {"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=16000:duration=2"});
    arguments.insert(arguments.end(), {"-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0[v]", "-map", "[v]"});
    if (audio)
        arguments.insert(arguments.end(), {"-map", "2:a", "-c:a", "aac"});
    arguments.insert(arguments.end(), {"-r", std::to_string(frameRate), "-c:v", codec, "-g", "4", "-pix_fmt", "yuv420p", "-threads", "1",
                                          "-y", revia::actions::PathToUtf8(target)});
    Run(settings.ffmpegPath, arguments);
}

std::string CpuH264Encoder(const VideoDecoderSettings& settings)
{
    const auto encoders = Run(settings.ffmpegPath, {"-hide_banner", "-encoders"});
    for (const char* name : {"libopenh264", "libx264"})
        if (std::regex_search(encoders, std::regex(std::string("\\b") + name + "\\b")))
            return name;
    return {};
}

void TestUnavailableToolsReleaseNoContent()
{
    Fixtures fixtures;
    VideoDecoder missing({fixtures.directory / "missing-ffmpeg.exe", fixtures.directory / "missing-ffprobe.exe"});
    const auto source = fixtures.directory / "absent.mp4";
    const auto probe = missing.Probe(source);
    const auto sampled = missing.Sample(source, {}, {0});
    const auto audio = missing.ExtractAudio(source, {}, 0, 1000);
    bool displayed = false;
    const auto preview = missing.StreamPreview(source, {}, 0,
        [&](std::int64_t, const std::string&)
        {
            displayed = true;
            return true;
        });
    Check(!missing.Availability().empty() && !probe.ok && !probe.error.empty() && !sampled.ok && sampled.frames.empty() && !audio.ok &&
              audio.wave.empty() && !preview.ok && !displayed,
        "Missing local decoder tools released media or reported success.");
}

void TestTimestampUtilities()
{
    Check(FormatVideoTimestamp(12507) == "00:12.507", "Video timestamps lost millisecond precision.");
    Check(FormatVideoTimestamp(3723004) == "01:02:03.004", "Long video timestamps lost hours.");
    Check(SampleVideoInterval(2000, 0, 2000) == std::vector<std::int64_t>({0, 666, 1332, 1999}),
        "Whole-clip sampling omitted its bounds or requested past EOF.");
    Check(SampleVideoInterval(2000, 1000, 1500, 3) == std::vector<std::int64_t>({1000, 1250, 1500}),
        "Interval sampling escaped its explicit bounds.");
    Check(SampleVideoInterval(2000, 1500, 1000).empty() && SampleVideoInterval(0, 0, 0).empty() &&
              SampleVideoInterval(2000, 0, 2000, 0).empty(),
        "An invalid sampling interval became a valid request.");
}

#ifndef _WIN32
void TestExitObservationDoesNotReap()
{
    revia::process::OwnedProcess process;
    revia::process::OwnedProcessRequest request;
    request.executable = "/bin/sh";
    request.arguments = {"-c", "printf '%s\\n' \"$$\"; exit 7"};
    std::string error;
    Check(process.Start(request, error), "The POSIX exit-observation child could not start: " + error);
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((process.Running() || !process.ExitCode()) && std::chrono::steady_clock::now() < deadline)
        output += process.Read(std::chrono::milliseconds(10));
    Check(process.ExitCode() == 7, "The POSIX child exit code was not observed.");
    const auto pid = static_cast<pid_t>(std::stoll(output));
    siginfo_t status{};
    Check(waitid(P_PID, static_cast<id_t>(pid), &status, WEXITED | WNOHANG | WNOWAIT) == 0 && status.si_pid == pid &&
              status.si_code == CLD_EXITED && status.si_status == 7,
        "ExitCode reaped the child before its process owner terminated, allowing premature PID reuse.");
    process.Terminate();
    int reaped = 0;
    errno = 0;
    Check(waitpid(pid, &reaped, WNOHANG) == -1 && errno == ECHILD && process.ExitCode() == 7,
        "Terminate did not remain the sole final child reaper or lost the observed exit code.");
}
#endif

void TestPortableDefaultResolution(const std::filesystem::path& clip, const std::filesystem::path& directory)
{
    const auto previous = std::filesystem::current_path();
    struct RestoreDirectory
    {
        std::filesystem::path previous;
        ~RestoreDirectory()
        {
            std::filesystem::current_path(previous);
        }
    } restore{previous};
    const auto externalRoot = directory / "external-runtime";
    std::filesystem::create_directories(externalRoot / "Config");
    std::filesystem::current_path(externalRoot);
    VideoDecoder decoder;
    Check(decoder.Availability().empty(), "The external runtime root hid installed decoder executables beside the application.");
    Check(decoder.Probe(clip).ok, "A default decoder in an external runtime root could not probe the actual local fixture.");
    VideoDecoderSettings explicitMissing{directory / "missing-ffmpeg.exe", directory / "missing-ffprobe.exe"};
    Check(!VideoDecoder(explicitMissing).Availability().empty(), "Explicit missing decoder paths silently fell back to other executables.");
}

void TestProbeAndLimits(const VideoDecoderSettings& settings, const std::filesystem::path& clip, const std::filesystem::path& directory)
{
    VideoDecoder decoder(settings);
    const auto probe = decoder.Probe(clip);
    Check(probe.ok && probe.info.mimeType == "video/mp4" && probe.info.videoCodec == "h264" && probe.info.width == 160 &&
              probe.info.height == 96 && probe.info.durationMs == 2000 && !probe.info.hasAudio,
        "The real silent MP4 was not probed correctly: " + probe.error);
    VideoLimits small;
    small.maxBytes = 16;
    Check(!decoder.Probe(clip, small).ok, "An oversized clip was admitted.");
    small = {};
    small.maxDurationMs = 1000;
    Check(!decoder.Probe(clip, small).ok, "An over-duration clip was admitted.");
    small = {};
    small.maxSide = 100;
    Check(!decoder.Probe(clip, small).ok, "An over-resolution clip was admitted.");
    small = {};
    small.maxPixels = 1000;
    Check(!decoder.Probe(clip, small).ok, "A clip above the pixel ceiling was admitted.");
    small = {};
    small.processTimeoutMs = 0;
    Check(!decoder.Probe(clip, small).ok, "Invalid decoder limits were accepted.");
    const auto corrupt = directory / "002.mp4";
    std::ofstream(corrupt, std::ios::binary) << "This is not a video.";
    Check(!decoder.Probe(corrupt).ok, "Corrupt video bytes were accepted by their extension.");
    Check(!decoder.Probe("https://example.com/clip.mp4").ok && !decoder.Probe(directory).ok,
        "A network URI or non-file was accepted as uploaded video.");
    std::stop_source cancellation;
    cancellation.request_stop();
    const auto cancelled = decoder.Probe(clip, {}, cancellation.get_token());
    Check(!cancelled.ok && cancelled.cancelled, "Pre-cancelled probing ran as success.");
    auto missing = settings;
    missing.ffmpegPath = directory / "missing.exe";
    Check(!VideoDecoder(missing).Availability().empty() && !VideoDecoder(missing).Probe(clip).ok,
        "A missing decoder was reported as available.");
    Run(settings.ffmpegPath, {"-this-option-does-not-exist"}, false);
}

void TestSampling(const VideoDecoderSettings& settings, const std::filesystem::path& clip, const std::filesystem::path& directory)
{
    VideoDecoder decoder(settings);
    const auto probe = decoder.Probe(clip);
    Check(probe.ok, "The sampling fixture did not probe: " + probe.error);
    const auto sampled = decoder.Sample(clip, probe.info, {120, 1120});
    Check(sampled.ok && sampled.frames.size() == 2, "Real frame sampling failed: " + sampled.error);
    Check(sampled.frames[0].timestampMs == 250 && sampled.frames[1].timestampMs == 1250,
        "Sample labels were requested times instead of the actual decoded frame timestamps.");
    for (std::size_t index = 0; index < sampled.frames.size(); ++index)
    {
        const auto& frame = sampled.frames[index];
        Check(frame.image.bytes && frame.image.mimeType == "image/png" && frame.image.width == 160 && frame.image.height == 96,
            "A sampled PNG lost its payload or dimensions.");
        const auto png = directory / ("frame-" + std::to_string(index) + ".png");
        std::ofstream output(png, std::ios::binary);
        output.write(frame.image.bytes->data(), static_cast<std::streamsize>(frame.image.bytes->size()));
        output.close();
        const auto rgb = Run(settings.ffmpegPath, {"-nostdin", "-v", "error", "-i", revia::actions::PathToUtf8(png), "-frames:v", "1",
                                                      "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"});
        Check(rgb.size() == 160 * 96 * 3, "The returned PNG did not independently decode.");
        const auto red = static_cast<unsigned char>(rgb[0]);
        const auto blue = static_cast<unsigned char>(rgb[2]);
        Check(index == 0 ? red > 200 && blue < 30 : blue > 200 && red < 30,
            "The requested timestamp returned pixels from the wrong part of the clip.");
    }
    const auto last = decoder.Sample(clip, probe.info, {1999});
    Check(last.ok && last.frames.size() == 1 && last.frames.front().timestampMs == 1750,
        "The end-of-clip request did not return the actual last available frame.");
    Check(!decoder.Sample(clip, probe.info, {0, 250, 500, 750, 1000}).ok && !decoder.Sample(clip, probe.info, {-1}).ok &&
              !decoder.Sample(clip, probe.info, {2000}).ok,
        "An invalid or over-budget sample request was accepted.");
    auto stale = probe.info;
    stale.width = 999;
    Check(!decoder.Sample(clip, stale, {0}).ok, "Stale clip metadata was trusted over source bytes.");
    VideoLimits limits;
    limits.frameSide = 64;
    const auto reduced = decoder.Sample(clip, probe.info, {0}, limits);
    Check(reduced.ok && reduced.frames.front().image.width == 64 && reduced.frames.front().image.height <= 64,
        "The sampled frame ignored its resize budget.");
}

void TestPreview(const VideoDecoderSettings& settings, const std::filesystem::path& clip)
{
    VideoDecoder decoder(settings);
    const auto probe = decoder.Probe(clip);
    Check(probe.ok, "The preview fixture did not probe.");
    std::vector<std::int64_t> timestamps;
    const auto preview = decoder.StreamPreview(clip, probe.info, 1000,
        [&](const std::int64_t timestamp, const std::string& bytes)
        {
            Check(bytes.size() == 640 * 360 * 3, "A preview frame was incomplete.");
            const std::size_t center = (180 * 640 + 320) * 3;
            Check(static_cast<unsigned char>(bytes[center + 2]) > 200 && static_cast<unsigned char>(bytes[center]) < 30,
                "A seeked preview showed the wrong interval.");
            timestamps.push_back(timestamp);
            return true;
        });
    Check(preview.ok && !preview.cancelled && preview.frames == timestamps.size() && timestamps.size() >= 10 &&
              timestamps.front() == 1000 && timestamps.back() < 2000 && std::is_sorted(timestamps.begin(), timestamps.end()),
        "Preview did not preserve bounded seek/timing: " + preview.error);
    std::size_t delivered = 0;
    const auto cancelled = decoder.StreamPreview(clip, probe.info, 0, [&](std::int64_t, const std::string&) { return ++delivered < 2; });
    Check(!cancelled.ok && cancelled.cancelled && delivered == 2, "Preview ignored cancellation/backpressure.");
    const auto paced = decoder.StreamPreview(clip, probe.info, 0,
        [](std::int64_t, const std::string&)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(70));
            return true;
        });
    Check(paced.ok && paced.frames >= 25, "A normally paced preview overflowed its bounded queue: " + paced.error);
    std::stop_source stop;
    const auto stopped = decoder.StreamPreview(
        clip, probe.info, 0,
        [&](std::int64_t, const std::string&)
        {
            stop.request_stop();
            return true;
        },
        stop.get_token());
    Check(stopped.cancelled && !stopped.ok && stopped.frames == 1, "An in-flight preview ignored its stop token.");
}

void TestFailureCleanup(const VideoDecoderSettings& settings, const std::filesystem::path& clip, const std::filesystem::path& directory)
{
    VideoDecoder decoder(settings);
    const auto probe = decoder.Probe(clip);
    Check(probe.ok, "The failure-control fixture did not probe.");
    std::ifstream input(clip, std::ios::binary);
    std::string damaged((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto payload = damaged.find("mdat");
    Check(payload != std::string::npos && payload >= 4 && payload + 4 < damaged.size(), "The fixture has no mutable packet payload.");
    std::uint32_t boxBytes = 0;
    for (std::size_t at = payload - 4; at < payload; ++at)
        boxBytes = (boxBytes << 8) | static_cast<unsigned char>(damaged[at]);
    Check(boxBytes >= 8 && boxBytes <= damaged.size() - payload + 4, "The fixture's media-data box is invalid.");
    std::fill(damaged.begin() + static_cast<std::ptrdiff_t>(payload + 4),
        damaged.begin() + static_cast<std::ptrdiff_t>(payload - 4 + boxBytes), static_cast<char>(0xff));
    const auto damagedClip = directory / "006.mp4";
    std::ofstream damagedFile(damagedClip, std::ios::binary);
    damagedFile.write(damaged.data(), static_cast<std::streamsize>(damaged.size()));
    damagedFile.close();
    const auto failed = decoder.Sample(damagedClip, probe.info, {0});
    Check(!failed.ok && failed.frames.empty(), "A corrupt packet produced accepted partial visual evidence.");
    auto wrongTool = settings;
    wrongTool.ffmpegPath = settings.ffprobePath;
    const auto nonzero = VideoDecoder(wrongTool).Sample(clip, probe.info, {0});
    Check(!nonzero.ok && nonzero.frames.empty(), "A decoder subprocess failure released a frame.");
    VideoLimits limits;
    limits.processTimeoutMs = 1;
    const auto started = std::chrono::steady_clock::now();
    const auto timedOut = decoder.Probe(clip, limits);
    Check(!timedOut.ok && std::chrono::steady_clock::now() - started < std::chrono::seconds(3),
        "A video probe exceeded its configured deadline or reported timed-out success.");
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = decoder.Sample(clip, probe.info, {0}, {}, stop.get_token());
    Check(cancelled.cancelled && !cancelled.ok && cancelled.frames.empty(), "Cancelled sampling released a frame.");
    const auto multiple = directory / "007.mp4";
    Run(settings.ffmpegPath, {"-nostdin", "-v", "error", "-i", revia::actions::PathToUtf8(clip), "-map", "0:v", "-map", "0:v", "-c", "copy",
                                 "-y", revia::actions::PathToUtf8(multiple)});
    Check(!decoder.Probe(multiple).ok, "Multiple video streams bypassed the single-video contract.");
    const auto highRate = directory / "008.webm";
    MakeClip(settings, highRate, "libvpx-vp9", false, 240);
    Check(!decoder.Probe(highRate).ok, "An excessive frame rate bypassed the decode ceiling.");
}

void TestAudioExtraction(const VideoDecoderSettings& settings, const std::filesystem::path& audioClip,
    const std::filesystem::path& silentClip, const VideoInfo& info)
{
    VideoDecoder decoder(settings);
    const auto audio = decoder.ExtractAudio(audioClip, info, 250, 750);
    Check(audio.ok && !audio.cancelled && audio.wave.size() == 16044 && audio.wave.substr(0, 4) == "RIFF" &&
              audio.wave.substr(8, 8) == "WAVEfmt " && audio.wave.substr(36, 4) == "data",
        "A known audio interval did not produce canonical 16 kHz mono WAV: " + audio.error);
    const auto little = [&](const std::size_t offset)
    {
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index)
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(audio.wave[offset + index])) << (index * 8);
        return value;
    };
    Check(little(4) == 16036 && little(24) == 16000 && little(28) == 32000 && little(40) == 16000 &&
              static_cast<unsigned char>(audio.wave[22]) == 1 && static_cast<unsigned char>(audio.wave[34]) == 16,
        "Audio metadata did not describe the exact interval's PCM bytes.");
    Check(std::any_of(audio.wave.begin() + 44, audio.wave.end(), [](const char byte) { return byte != 0; }),
        "Known non-silent speech evidence became silence.");
    const auto silent = decoder.Probe(silentClip);
    const auto noAudio = decoder.ExtractAudio(silentClip, silent.info, 0, 1000);
    Check(!noAudio.ok && noAudio.wave.empty(), "A silent clip invented an audio transcript source.");
    Check(!decoder.ExtractAudio(audioClip, info, -1, 1000).ok && !decoder.ExtractAudio(audioClip, info, 1000, 1000).ok &&
              !decoder.ExtractAudio(audioClip, info, 0, 60001).ok,
        "Invalid audio intervals bypassed bounds.");
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = decoder.ExtractAudio(audioClip, info, 0, 1000, stop.get_token());
    Check(cancelled.cancelled && !cancelled.ok && cancelled.wave.empty(), "Cancelled extraction released audio evidence.");
}
int RunVideoDecoderChecks()
{
#ifndef _WIN32
    TestExitObservationDoesNotReap();
#endif
    TestTimestampUtilities();
    TestUnavailableToolsReleaseNoContent();
    const auto settings = DecoderSettings();
    const auto availability = VideoDecoder(settings).Availability();
    if (!availability.empty())
    {
        std::cout << "SKIP: Actual video decoder checks require local FFmpeg/ffprobe. " << availability << '\n';
        return 77;
    }
    const auto h264 = CpuH264Encoder(settings);
    if (h264.empty())
    {
        std::cout << "SKIP: Actual video decoder fixtures require the CPU libopenh264 or libx264 encoder; neither is installed.\n";
        return 77;
    }
    std::cout << "CPU H.264 fixture encoder: " << h264 << '\n';
    Fixtures fixtures;
    const auto clip = fixtures.directory / "001 ' quoted.mp4";
    MakeClip(settings, clip, h264);
    if (VideoDecoder().Availability().empty())
        TestPortableDefaultResolution(clip, fixtures.directory);
    else
        std::cout << "Default installation lookup check was not run; explicit test executables supply this real-tool suite.\n";
    TestProbeAndLimits(settings, clip, fixtures.directory);
    TestSampling(settings, clip, fixtures.directory);
    TestPreview(settings, clip);
    TestFailureCleanup(settings, clip, fixtures.directory);
    const auto webm = fixtures.directory / "003.webm";
    MakeClip(settings, webm, "libvpx-vp9");
    const auto webmProbe = VideoDecoder(settings).Probe(webm);
    Check(webmProbe.ok && webmProbe.info.mimeType == "video/webm" && webmProbe.info.videoCodec == "vp9",
        "A real VP9 WebM failed probing: " + webmProbe.error);
    const auto webmFrames = VideoDecoder(settings).Sample(webm, webmProbe.info, {120, 1120});
    Check(webmFrames.ok && webmFrames.frames.size() == 2 && webmFrames.frames[0].timestampMs == 250 &&
              webmFrames.frames[1].timestampMs == 1250,
        "WebM interval sampling lost actual frame timestamps: " + webmFrames.error);
    const auto lowRate = fixtures.directory / "009.mp4";
    MakeClip(settings, lowRate, h264, false, 1);
    const auto lowProbe = VideoDecoder(settings).Probe(lowRate);
    Check(lowProbe.ok, "A supported one-fps clip failed probing.");
    const auto lowPreview = VideoDecoder(settings).StreamPreview(lowRate, lowProbe.info, 0,
        [](std::int64_t, const std::string&)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(70));
            return true;
        });
    Check(lowPreview.ok && lowPreview.frames == 30, "A low-rate preview overflowed on duplicated frames: " + lowPreview.error);
    const auto audio = fixtures.directory / "004.mp4";
    MakeClip(settings, audio, h264, true);
    const auto audioProbe = VideoDecoder(settings).Probe(audio);
    Check(audioProbe.ok && audioProbe.info.hasAudio && audioProbe.info.audioCodec == "aac",
        "Audio presence was not kept separate from visual metadata: " + audioProbe.error);
    TestAudioExtraction(settings, audio, clip, audioProbe.info);
    const auto unsupported = fixtures.directory / "005.mp4";
    MakeClip(settings, unsupported, "mpeg4");
    Check(!VideoDecoder(settings).Probe(unsupported).ok, "An unsupported MP4 video codec was admitted.");
    std::cout << "Real MP4/WebM probing, timestamped pixels, silent preview, audio metadata, limits and cancellation passed.\n";
    return 0;
}
}

void RunVideoDecoderTests()
{
    if (RunVideoDecoderChecks() == 77)
        throw std::runtime_error("Actual video checks were skipped; use the standalone target to report skip status 77.");
}

#ifdef REVIA_VIDEO_DECODER_STANDALONE
int main()
{
    try
    {
        return RunVideoDecoderChecks();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#endif
