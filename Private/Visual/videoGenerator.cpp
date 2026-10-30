#include "Visual/videoGenerator.h"

#include "Audit/contentDigest.h"
#include "Core/localApiKey.h"
#include "Core/runtimePath.h"
#include "Process/ownedProcess.h"
#include "Visual/imageArtifact.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::visual
{
namespace
{
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr std::size_t MaximumVideoBytes = 64 * 1024 * 1024;

std::string Text(const std::filesystem::path& path)
{
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path Path(const std::string& value)
{
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

bool Plain(const std::filesystem::path& path)
{
    if (!path.is_absolute())
        return false;
#ifdef _WIN32
    if (path.native().starts_with(L"\\\\") || GetDriveTypeW(path.root_path().c_str()) == DRIVE_REMOTE)
        return false;
#endif
    for (auto part = path; !part.empty(); part = part.parent_path())
    {
        std::error_code error;
        if (std::filesystem::is_symlink(part, error))
            return false;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(part.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
#endif
        if (part == part.root_path())
            break;
    }
    return true;
}

bool Inside(const std::filesystem::path& path, const std::filesystem::path& root)
{
#ifdef _WIN32
    const auto value = path.lexically_normal().generic_wstring();
    const auto prefix = root.lexically_normal().generic_wstring();
    return value.size() >= prefix.size() &&
           CompareStringOrdinal(value.data(), static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(prefix.size()), TRUE) ==
               CSTR_EQUAL &&
           (value.size() == prefix.size() || value[prefix.size()] == L'/');
#else
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
#endif
}

std::filesystem::path ScratchRoot(const std::filesystem::path& outputRoot)
{
    std::error_code error;
    auto temporary = std::filesystem::temp_directory_path(error);
    if (error || !Plain(temporary))
        return {};
    temporary = std::filesystem::canonical(temporary, error);
    if (error)
        return {};
#ifdef _WIN32
    for (const auto* name : {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"})
    {
        std::wstring value(32768, L'\0');
        const auto size = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        if (size >= value.size())
            return {};
        if (size)
        {
            value.resize(size);
            if (Inside(temporary, std::filesystem::path(value)))
                return {};
        }
    }
#endif
    const auto jobs = temporary / "ReviaVideoJobs";
    if (!Plain(jobs) || Inside(jobs, outputRoot) || Inside(outputRoot, jobs))
        return {};
    std::filesystem::create_directory(jobs, error);
    if (error || !Plain(jobs) || !std::filesystem::is_directory(jobs, error) || error)
        return {};
    return jobs;
}

std::filesystem::path Installed(const std::string& value)
{
    auto path = Path(value);
    if (path.empty())
        return {};
    if (!path.is_absolute())
    {
        auto found = core::FindInstalledArtifact(core::RuntimeRoot(), path);
        path = found.empty() ? core::ResolveRuntimePath(path) : found;
    }
    std::error_code error;
    return Plain(path) && std::filesystem::is_regular_file(path, error) && !error ? std::filesystem::canonical(path, error)
                                                                                  : std::filesystem::path{};
}

bool Read(const std::filesystem::path& path, const std::size_t limit, std::string& value)
{
    std::error_code error;
    if (!Plain(path) || !std::filesystem::is_regular_file(path, error) || error)
        return false;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > limit)
        return false;
    std::ifstream input(path, std::ios::binary);
    value.resize(static_cast<std::size_t>(size) + 1);
    input.read(value.data(), static_cast<std::streamsize>(value.size()));
    value.resize(static_cast<std::size_t>(input.gcount()));
    return !input.bad() && value.size() == size;
}

bool Write(const std::filesystem::path& path, const std::string& value)
{
    if (!Plain(path) || std::filesystem::exists(path))
        return false;
    std::ofstream output(path, std::ios::binary);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    output.close();
    return static_cast<bool>(output);
}

bool Digest(const std::string& value)
{
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string Mode(const VideoRequestMode mode)
{
    return mode == VideoRequestMode::ImageToVideo ? "image_to_video" : "text_to_video";
}

std::string Availability(const videoSettings& settings)
{
    if (settings.width != 384 || settings.height != 256 || settings.frameCount != 49 || settings.fps != 25 || settings.steps != 50 ||
        settings.gpuIndex != 1 || settings.cpuThreads != 6 || settings.requestTimeoutSeconds < 1 || settings.requestTimeoutSeconds > 600)
        return "Only the qualified secondary-GPU LTX configuration is supported.";
    if (settings.modelDirectory.empty() || settings.runtimeOverlayDirectory.empty() || settings.outputPath.empty())
        return "The approved local LTX model and isolated runtime have not been selected.";
    const auto model = core::ResolveRuntimePath(Path(settings.modelDirectory));
    const auto overlay = core::ResolveRuntimePath(Path(settings.runtimeOverlayDirectory));
    std::error_code error;
    const bool overlayPresent = std::filesystem::is_directory(overlay, error) && !error;
    error.clear();
    const bool modelPresent = std::filesystem::is_regular_file(model / "model_index.json", error) && !error;
    if (!Plain(model) || !Plain(overlay) || !overlayPresent || !modelPresent)
        return "The selected local LTX installation or isolated runtime is unavailable.";
    if (Installed(settings.pythonExecutable).empty() || Installed(settings.workerScript).empty() ||
        Installed(settings.ffmpegPath).empty() || Installed(settings.ffprobePath).empty())
        return "The local video Python worker, FFmpeg or FFprobe executable is unavailable.";
    return {};
}

bool WithinJobBound(const std::filesystem::path& directory)
{
    std::error_code error;
    std::uintmax_t bytes = 0;
    std::size_t count = 0;
    for (std::filesystem::recursive_directory_iterator iterator(directory, error), end; !error && iterator != end;
        iterator.increment(error))
    {
        if (++count > 512 || !Plain(iterator->path()))
            return false;
        if (iterator->is_regular_file(error))
        {
            bytes += iterator->file_size(error);
            if (bytes > MaximumVideoBytes)
                return false;
        }
        else if (!iterator->is_directory(error))
            return false;
    }
    return !error;
}

bool RemoveJob(const std::filesystem::path& directory)
{
    std::error_code error;
    if (!std::filesystem::exists(directory, error))
        return !error;
    if (!Plain(directory))
        return false;
    std::vector<std::filesystem::path> paths;
    for (std::filesystem::recursive_directory_iterator iterator(directory, error), end; !error && iterator != end;
        iterator.increment(error))
    {
        if (paths.size() >= 1024 || !Plain(iterator->path()))
            return false;
        paths.push_back(iterator->path());
    }
    if (error)
        return false;
    for (auto iterator = paths.rbegin(); iterator != paths.rend(); ++iterator)
    {
        std::filesystem::remove(*iterator, error);
        if (error)
            return false;
    }
    return std::filesystem::remove(directory, error) && !error;
}

bool RemoveOutput(const std::filesystem::path& path)
{
    std::error_code error;
    if (!Plain(path) || std::filesystem::is_directory(path, error))
        return false;
    error.clear();
    std::filesystem::remove(path, error);
    return !error;
}

bool Run(const std::filesystem::path& executable, const std::vector<std::string>& arguments, const std::size_t limit,
    const std::stop_token token, const std::function<bool()>& admission, std::string& output, std::string& error)
{
    process::OwnedProcess child;
    process::OwnedProcessRequest request;
    request.executable = executable;
    request.arguments = arguments;
    request.maxPendingOutputBytes = limit + 1;
    request.maxErrorBytes = 8192;
    if (token.stop_requested() || (admission && !admission()))
    {
        error = "Video verification was cancelled.";
        return false;
    }
    if (!child.Start(request, error))
        return false;
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (true)
    {
        if (token.stop_requested() || (admission && !admission()) || Clock::now() >= deadline)
        {
            child.Terminate();
            error = "Video verification was cancelled or exceeded its time limit.";
            return false;
        }
        const auto bytes = child.Read(std::chrono::milliseconds(20));
        if (bytes.size() > limit - output.size())
        {
            child.Terminate();
            error = "Video verification exceeded its output bound.";
            return false;
        }
        output += bytes;
        if (!child.Running() && child.ExitCode())
            break;
        if (!child.Running())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (child.ExitCode() != 0 || !child.StopReason().empty())
    {
        error = "The generated video could not be completely decoded.";
        return false;
    }
    return true;
}

std::int64_t Integer(const Json& json, const char* name, const std::int64_t maximum)
{
    const auto& value = json.at(name);
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(maximum)))
        throw std::runtime_error("Invalid video receipt integer.");
    const auto integer = value.get<std::int64_t>();
    if (integer < 0 || integer > maximum)
        throw std::runtime_error("Out-of-range video receipt integer.");
    return integer;
}

VideoArtifactReceipt ParseReceipt(const Json& json)
{
    VideoArtifactReceipt result;
    result.path = Path(json.at("path").get<std::string>());
    result.sha256 = json.at("sha256").get<std::string>();
    result.jobId = json.at("jobId").get<std::string>();
    result.model = json.at("model").get<std::string>();
    result.revision = json.at("revision").get<std::string>();
    const auto mode = json.at("mode").get<std::string>();
    if (mode != "image_to_video" && mode != "text_to_video")
        throw std::runtime_error("Unsupported video receipt mode.");
    result.mode = mode == "image_to_video" ? VideoRequestMode::ImageToVideo : VideoRequestMode::TextToVideo;
    result.seed = static_cast<std::uint32_t>(Integer(json, "seed", 0xffffffffLL));
    result.sourceId = json.at("sourceId").get<std::string>();
    result.sourceDigest = json.at("sourceDigest").get<std::string>();
    result.sourcePreparation = json.at("sourcePreparation").get<std::string>();
    result.preparedSourceDigest = json.at("preparedSourceDigest").get<std::string>();
    result.width = static_cast<int>(Integer(json, "width", 384));
    result.height = static_cast<int>(Integer(json, "height", 256));
    result.frameCount = static_cast<int>(Integer(json, "frameCount", 49));
    result.fps = static_cast<int>(Integer(json, "fps", 25));
    result.durationMs = Integer(json, "durationMs", 1960);
    result.steps = static_cast<int>(Integer(json, "steps", 50));
    result.denoisingSteps = static_cast<int>(Integer(json, "denoisingSteps", 50));
    result.provider = json.at("provider").get<std::string>();
    result.pipeline = json.at("pipeline").get<std::string>();
    result.device = json.at("device").get<std::string>();
    result.dtype = json.at("dtype").get<std::string>();
    result.offload = json.at("offload").get<std::string>();
    result.cpuThreads = static_cast<int>(Integer(json, "cpuThreads", 6));
    result.modelLoadMilliseconds = json.at("modelLoadMilliseconds").get<double>();
    result.inferenceMilliseconds = json.at("inferenceMilliseconds").get<double>();
    result.encodingMilliseconds = json.at("encodingMilliseconds").get<double>();
    result.workerPid = static_cast<std::uint64_t>(Integer(json, "workerPid", 0xffffffffLL));
    result.peakWorkerRssMiB = json.at("peakWorkerRssMiB").get<double>();
    result.minimumFreeSystemRamBytes = static_cast<std::uint64_t>(Integer(json, "minimumFreeSystemRamBytes", 1LL << 50));
    result.processTreeMemoryMeasured = json.at("processTreeMemoryMeasured").get<bool>();
    return result;
}
}

bool VerifyVideoArtifact(const VideoArtifactExpectation& expected, const VideoArtifactReceipt& receipt,
    const attachments::VideoDecoderSettings& decoderSettings, const std::stop_token stopToken, const std::function<bool()>& admission,
    std::string& outError)
{
    outError = "The generated video's ownership, mode or qualified parameters do not match this request.";
    if (stopToken.stop_requested() || (admission && !admission()) || expected.jobId.empty() || expected.model != VideoModel ||
        expected.revision != VideoModelRevision || expected.jobId.size() > 96 ||
        expected.path.filename() != "video-" + expected.jobId + ".mp4" || !Plain(expected.path) || !Plain(receipt.path) ||
        expected.path != receipt.path || receipt.jobId != expected.jobId || receipt.model != expected.model ||
        receipt.revision != expected.revision || receipt.mode != expected.mode || receipt.seed != expected.seed ||
        receipt.sourceId != expected.sourceId || receipt.sourceDigest != expected.sourceDigest ||
        receipt.sourcePreparation != expected.sourcePreparation || expected.width != 384 || expected.height != 256 ||
        expected.frameCount != 49 || expected.fps != 25 || expected.durationMs != 1960 || expected.steps != 50 || receipt.width != 384 ||
        receipt.height != 256 || receipt.frameCount != 49 || receipt.fps != 25 || receipt.durationMs != 1960 || receipt.steps != 50 ||
        receipt.denoisingSteps != 50 || receipt.provider != "local_diffusers_ltx" || receipt.device != "cuda:1" ||
        receipt.dtype != "float32" || receipt.offload != "sequential_cpu" || receipt.cpuThreads != 6 || !Digest(receipt.sha256) ||
        !std::isfinite(receipt.modelLoadMilliseconds) || receipt.modelLoadMilliseconds < 0 ||
        !std::isfinite(receipt.inferenceMilliseconds) || receipt.inferenceMilliseconds < 0 ||
        !std::isfinite(receipt.encodingMilliseconds) || receipt.encodingMilliseconds < 0 || receipt.workerPid == 0 ||
        !std::isfinite(receipt.peakWorkerRssMiB) || receipt.peakWorkerRssMiB <= 0 || receipt.processTreeMemoryMeasured ||
        receipt.minimumFreeSystemRamBytes < 8ULL * 1024 * 1024 * 1024)
        return false;
    if (expected.mode == VideoRequestMode::ImageToVideo)
    {
        if (expected.sourceId.empty() || expected.sourceId.size() > 128 || expected.sourceId.find('\0') != std::string::npos ||
            !Digest(expected.sourceDigest) || expected.sourcePreparation != "contain-pad-rgb-black-v1" ||
            !Digest(receipt.preparedSourceDigest) || receipt.pipeline != "LTXImageToVideoPipeline")
            return false;
    }
    else if (expected.mode != VideoRequestMode::TextToVideo || !expected.sourceId.empty() || !expected.sourceDigest.empty() ||
             !expected.sourcePreparation.empty() || !receipt.preparedSourceDigest.empty() || receipt.pipeline != "LTXPipeline")
        return false;
    std::string bytes;
    if (!Read(expected.path, MaximumVideoBytes, bytes) || audit::ContentDigest(bytes) != receipt.sha256)
    {
        outError = "The generated video bytes do not match the bounded artifact receipt.";
        return false;
    }
    attachments::VideoDecoder decoder(decoderSettings);
    attachments::VideoLimits limits;
    limits.maxDurationMs = 3000;
    limits.processTimeoutMs = 2000;
    const auto probe = decoder.Probe(expected.path, limits, stopToken);
    if (!probe.ok || probe.info.mimeType != "video/mp4" || probe.info.videoCodec != "h264" || probe.info.hasAudio ||
        probe.info.width != 384 || probe.info.height != 256 || std::abs(probe.info.durationMs - 1960) > 1)
    {
        outError = "The generated artifact is not the expected silent MP4/H.264 stream.";
        return false;
    }
    auto ffmpeg = decoderSettings.ffmpegPath.empty() ? Installed("ThirdParty/ffmpeg/bin/ffmpeg.exe") : decoderSettings.ffmpegPath;
    auto ffprobe = decoderSettings.ffprobePath.empty() ? Installed("ThirdParty/ffmpeg/bin/ffprobe.exe") : decoderSettings.ffprobePath;
    const std::vector<std::string> base{"-v", "error", "-protocol_whitelist", "file", "-format_whitelist", "mov", "-threads", "1",
        "-enable_drefs", "0", "-use_absolute_path", "0"};
    auto arguments = base;
    arguments.insert(arguments.end(), {"-select_streams", "v:0", "-show_frames", "-show_entries", "frame=best_effort_timestamp_time", "-of",
                                          "json", Text(expected.path)});
    std::string timestamps;
    if (!Run(ffprobe, arguments, 65536, stopToken, admission, timestamps, outError))
        return false;
    try
    {
        const auto frames = Json::parse(timestamps).at("frames");
        if (!frames.is_array() || frames.size() != 49)
            throw std::runtime_error("Unexpected frame count.");
        for (std::size_t index = 0; index < frames.size(); ++index)
        {
            const auto text = frames[index].at("best_effort_timestamp_time").get<std::string>();
            std::size_t consumed = 0;
            const double pts = std::stod(text, &consumed);
            if (consumed != text.size() || !std::isfinite(pts) || std::abs(pts - index / 25.0) > 0.001)
                throw std::runtime_error("Unexpected frame timestamp.");
        }
    }
    catch (...)
    {
        outError = "The MP4 does not contain the expected 49 ordered frame timestamps.";
        return false;
    }
    arguments = base;
    arguments.insert(arguments.end(), {"-nostdin", "-xerror", "-err_detect", "explode", "-i", Text(expected.path), "-map", "0:v:0",
                                          "-frames:v", "50", "-vsync", "0", "-an", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"});
    std::string decoded;
    if (!Run(ffmpeg, arguments, 384U * 256U * 3U * 50U, stopToken, admission, decoded, outError))
        return false;
    if (decoded.size() != 384U * 256U * 3U * 49U)
    {
        outError = "The MP4 could not be fully decoded into exactly 49 bounded frames.";
        return false;
    }
    if (!Read(expected.path, MaximumVideoBytes, bytes) || audit::ContentDigest(bytes) != receipt.sha256 || stopToken.stop_requested() ||
        (admission && !admission()))
    {
        outError = "The video changed or its admission retired during verification.";
        return false;
    }
    outError.clear();
    return true;
}

VideoGenerator::~VideoGenerator()
{
    Shutdown();
}

void VideoGenerator::Configure(videoSettings settings)
{
    Cancel();
    std::lock_guard lock(mutex);
    {
        std::lock_guard statusLock(statusMutex);
        configuration = std::move(settings);
        enabled.store(configuration.bEnabled);
        snapshot = {configuration.bEnabled ? "idle" : "disabled", {}, {}};
    }
    shuttingDown.store(false);
}

bool VideoGenerator::IsEnabled() const
{
    return enabled.load();
}

bool VideoGenerator::IsAvailable(std::string& detail)
{
    videoSettings settings;
    {
        std::lock_guard lock(statusMutex);
        settings = configuration;
    }
    detail = settings.bEnabled ? Availability(settings) : "Video generation is disabled.";
    return detail.empty();
}

std::filesystem::path VideoGenerator::OutputDirectory()
{
    std::string output;
    {
        std::lock_guard lock(statusMutex);
        output = configuration.outputPath;
    }
    return core::ResolveRuntimeWritePath(Path(output));
}

VideoResult VideoGenerator::Generate(const VideoRequest& request, const std::stop_token stopToken, std::function<bool()> admission,
    const std::filesystem::path& expectedOutputRoot)
{
    VideoResult result;
    const auto generation = cancellationGeneration.load();
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        result.message = "The local video provider is busy with another job.";
        return result;
    }
    std::stop_source source;
    videoSettings settings;
    {
        std::lock_guard statusLock(statusMutex);
        activeStop = source;
        settings = configuration;
    }
    std::stop_callback callerStop(stopToken, [&]() { source.request_stop(); });
    bool retired = false;
    const auto cancelled = [&]()
    {
        retired = retired || source.stop_requested() || shuttingDown.load() || generation != cancellationGeneration.load() ||
                  (admission && !admission());
        if (retired)
            source.request_stop();
        return retired;
    };
    if (cancelled())
    {
        result.cancelled = true;
        result.message = "Video generation was cancelled before it started.";
        return result;
    }
    result.message = settings.bEnabled ? Availability(settings) : "Video generation is disabled.";
    if (!result.message.empty())
        return result;
    if (request.prompt.empty() || request.prompt.size() > 8192 ||
        (request.mode == VideoRequestMode::ImageToVideo && (!request.source || !VerifyImageSource(*request.source, result.message))) ||
        (request.mode == VideoRequestMode::TextToVideo && request.source) ||
        (request.mode != VideoRequestMode::TextToVideo && request.mode != VideoRequestMode::ImageToVideo))
    {
        if (result.message.empty())
            result.message = "A bounded prompt and mode-appropriate static reference are required.";
        return result;
    }
    const auto started = Clock::now();
    auto& expected = result.expectedArtifact;
    expected.jobId = core::GenerateLocalApiKey();
    expected.mode = request.mode;
    expected.seed = request.seed ? *request.seed : std::random_device{}();
    if (request.source)
    {
        expected.sourceId = request.source->id;
        expected.sourceDigest = request.source->digest;
        expected.sourcePreparation = "contain-pad-rgb-black-v1";
    }
    std::error_code error;
    auto root = core::ResolveRuntimeWritePath(Path(settings.outputPath));
    if (!Plain(root))
    {
        result.message = "The admitted video output directory is unavailable or changed.";
        return result;
    }
    std::filesystem::create_directories(root, error);
    root = std::filesystem::weakly_canonical(root, error);
    if (error || !Plain(root) || (!expectedOutputRoot.empty() && root != expectedOutputRoot))
    {
        result.message = "The admitted video output directory is unavailable or changed.";
        return result;
    }
    expected.path = root / ("video-" + expected.jobId + ".mp4");
    // Source pixels, decoded frames and runtime caches must not enter a synced output tree.
    const auto jobs = ScratchRoot(root);
    const auto directory = jobs / expected.jobId;
    if (jobs.empty() || !std::filesystem::create_directory(directory, error) || error || !Plain(directory))
    {
        result.message = "A private owned video job directory could not be created.";
        return result;
    }
    process::OwnedProcess child;
    const auto deadline = started + std::chrono::seconds(settings.requestTimeoutSeconds);
    std::atomic<bool> timedOut = false;
    std::jthread timeout(
        [&](const std::stop_token token)
        {
            std::mutex waitMutex;
            std::unique_lock waitLock(waitMutex);
            std::condition_variable_any condition;
            condition.wait_until(waitLock, token, deadline, []() { return false; });
            if (!token.stop_requested())
            {
                timedOut.store(true);
                source.request_stop();
            }
        });
    try
    {
        Json body = {{"jobId", expected.jobId}, {"mode", Mode(request.mode)}, {"prompt", request.prompt}, {"seed", expected.seed},
            {"width", 384}, {"height", 256}, {"frameCount", 49}, {"fps", 25}, {"steps", 50}, {"outputRoot", Text(root)},
            {"scratchRoot", Text(jobs)}, {"jobDirectory", Text(directory)}, {"outputPath", Text(expected.path)},
            {"modelDirectory", Text(core::ResolveRuntimePath(Path(settings.modelDirectory)))},
            {"runtimeOverlayDirectory", Text(core::ResolveRuntimePath(Path(settings.runtimeOverlayDirectory)))},
            {"ffmpegPath", Text(Installed(settings.ffmpegPath))}};
        if (request.source)
        {
            if (!Write(directory / "source.bin", *request.source->bytes))
                throw std::runtime_error("The owned image source snapshot could not be created.");
            std::string actual;
            if (!Read(directory / "source.bin", 8 * 1024 * 1024, actual) || audit::ContentDigest(actual) != request.source->digest)
                throw std::runtime_error("The owned image source snapshot changed before submission.");
            body["source"] = {{"path", "source.bin"}, {"id", expected.sourceId}, {"sha256", expected.sourceDigest},
                {"mimeType", request.source->mimeType}, {"width", request.source->width}, {"height", request.source->height}};
        }
        if (!Write(directory / "job.json", body.dump()))
            throw std::runtime_error("The bounded video request could not be written.");
        process::OwnedProcessRequest processRequest;
        processRequest.executable = Installed(settings.pythonExecutable);
        processRequest.arguments = {Text(Installed(settings.workerScript)), "--job", Text(directory / "job.json")};
        processRequest.workingDirectory = directory;
        processRequest.maxPendingOutputBytes = 131072;
        processRequest.maxErrorBytes = 8192;
        processRequest.environment = {{"PYTHONPATH", body.at("runtimeOverlayDirectory").get<std::string>()}, {"PYTHONNOUSERSITE", "1"},
            {"PYTHONSAFEPATH", "1"}, {"PYTHONDONTWRITEBYTECODE", "1"}, {"HF_HUB_OFFLINE", "1"}, {"TRANSFORMERS_OFFLINE", "1"},
            {"HF_DATASETS_OFFLINE", "1"}, {"TOKENIZERS_PARALLELISM", "false"}, {"CUDA_DEVICE_ORDER", "PCI_BUS_ID"},
            {"CUDA_CACHE_MAXSIZE", "33554432"}};
        for (const auto* name : {"TEMP", "TMP", "HF_HOME", "TORCH_HOME", "XDG_CACHE_HOME", "CUDA_CACHE_PATH"})
        {
            const auto cache = directory / name;
            std::filesystem::create_directory(cache, error);
            if (error)
                throw std::runtime_error("The private video runtime scratch directory is unavailable.");
            processRequest.environment[name] = Text(cache);
        }
        if (cancelled())
            throw std::runtime_error("Video generation was cancelled before submission.");
        SetSnapshot({"loading", expected.jobId, {}});
        if (!child.Start(processRequest, result.message))
            throw std::runtime_error(result.message);
        std::string pending;
        std::size_t total = 0;
        while (true)
        {
            if (cancelled())
                throw std::runtime_error("Video generation was cancelled.");
            if (Clock::now() >= deadline)
                throw std::runtime_error("Video generation exceeded its time limit.");
            if (!WithinJobBound(directory))
                throw std::runtime_error("The video job exceeded its private scratch bound.");
            const auto resultPath = directory / "result.json";
            if (std::filesystem::exists(resultPath, error) && (!Plain(resultPath) || !std::filesystem::is_regular_file(resultPath) ||
                                                                  std::filesystem::file_size(resultPath, error) > 65536 || error))
                throw std::runtime_error("The video result exceeded its metadata bound.");
            auto bytes = child.Read(std::chrono::milliseconds(20));
            total += bytes.size();
            if (total > 131072)
                throw std::runtime_error("The video worker exceeded its status output bound.");
            pending += bytes;
            for (auto newline = pending.find('\n'); newline != std::string::npos; newline = pending.find('\n'))
            {
                try
                {
                    const auto progress = Json::parse(pending.substr(0, newline));
                    SetSnapshot(
                        {progress.value("state", "generating"), expected.jobId, {}, static_cast<int>(Integer(progress, "step", 50)), 50});
                }
                catch (...)
                {
                }
                pending.erase(0, newline + 1);
            }
            if (!child.Running() && child.ExitCode())
                break;
            if (!child.Running())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (child.ExitCode() != 0 || !child.StopReason().empty())
        {
            result.detail = child.FinalErrorOutput(std::chrono::milliseconds(100));
            throw std::runtime_error("The owned local video worker failed; no result was published.");
        }
        std::string receipt;
        if (!Read(directory / "result.json", 65536, receipt))
            throw std::runtime_error("The owned video worker did not return bounded result metadata.");
        result.receipt = ParseReceipt(Json::parse(receipt));
        SetSnapshot({"verifying", expected.jobId, {}, 50, 50});
        const attachments::VideoDecoderSettings decoder{Installed(settings.ffmpegPath), Installed(settings.ffprobePath)};
        if (!VerifyVideoArtifact(expected, result.receipt, decoder, source.get_token(), [&]() { return !cancelled(); }, result.message))
            throw std::runtime_error(result.message);
        if (cancelled())
            throw std::runtime_error("Video generation was cancelled before publication.");
        result.succeeded = true;
        result.path = expected.path;
        result.message = "Generated and verified the silent video file.";
        result.detail = "49 decoded frames at 384x256, 25 fps, 1.96 seconds; secondary-GPU FP32 sequential CPU offload. Visual quality "
                        "requires review.";
    }
    catch (const std::exception& failure)
    {
        child.Terminate();
        result.succeeded = false;
        result.cancelled = cancelled() && !timedOut.load();
        result.message = timedOut.load() ? "Video generation exceeded its time limit." : failure.what();
        result.path.clear();
        if (!RemoveOutput(expected.path))
            result.message += " The failed owned output could not be removed.";
    }
    child.Terminate();
    if (!RemoveJob(directory))
    {
        result.succeeded = false;
        result.path.clear();
        result.message += " The private video job could not be completely cleaned up.";
        if (!RemoveOutput(expected.path))
            result.message += " The failed owned output could not be removed.";
    }
    if (result.succeeded && cancelled())
    {
        result.succeeded = false;
        result.cancelled = !timedOut.load();
        result.path.clear();
        result.message =
            timedOut.load() ? "Video generation exceeded its time limit." : "Video generation was cancelled before publication.";
        if (!RemoveOutput(expected.path))
            result.message += " The cancelled owned output could not be removed.";
    }
    result.elapsedMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    SetSnapshot({result.succeeded   ? "succeeded"
                 : result.cancelled ? "cancelled"
                                    : "failed",
        expected.jobId, result.message, result.succeeded ? 50 : 0, 50});
    return result;
}

void VideoGenerator::Cancel()
{
    cancellationGeneration.fetch_add(1);
    std::stop_source source;
    {
        std::lock_guard lock(statusMutex);
        source = activeStop;
    }
    source.request_stop();
}

VideoJobSnapshot VideoGenerator::Snapshot() const
{
    std::lock_guard lock(statusMutex);
    return snapshot;
}

void VideoGenerator::SetSnapshot(VideoJobSnapshot value)
{
    std::lock_guard lock(statusMutex);
    snapshot = std::move(value);
}

void VideoGenerator::Shutdown()
{
    shuttingDown.store(true);
    Cancel();
    std::lock_guard lock(mutex);
}
}
