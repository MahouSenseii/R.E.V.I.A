#pragma once

#include "Attachments/videoDecoder.h"
#include "Visual/videoRequest.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

namespace revia::visual
{
inline constexpr const char* VideoModel = "Lightricks/LTX-Video-0.9.5";
inline constexpr const char* VideoModelRevision = "e58e28c39631af4d1468ee57a853764e11c1d37e";

struct VideoArtifactExpectation
{
    std::filesystem::path path;
    std::string jobId;
    std::string model = VideoModel;
    std::string revision = VideoModelRevision;
    VideoRequestMode mode = VideoRequestMode::TextToVideo;
    std::uint32_t seed = 0;
    std::string sourceId;
    std::string sourceDigest;
    std::string sourcePreparation;
    int width = 384;
    int height = 256;
    int frameCount = 49;
    int fps = 25;
    std::int64_t durationMs = 1960;
    int steps = 50;
};

struct VideoArtifactReceipt
{
    std::filesystem::path path;
    std::string sha256;
    std::string jobId;
    std::string model;
    std::string revision;
    VideoRequestMode mode = VideoRequestMode::TextToVideo;
    std::uint32_t seed = 0;
    std::string sourceId;
    std::string sourceDigest;
    std::string sourcePreparation;
    std::string preparedSourceDigest;
    int width = 0;
    int height = 0;
    int frameCount = 0;
    int fps = 0;
    std::int64_t durationMs = 0;
    int steps = 0;
    int denoisingSteps = 0;
    std::string provider;
    std::string pipeline;
    std::string device;
    std::string dtype;
    std::string offload;
    int cpuThreads = 0;
    double modelLoadMilliseconds = 0;
    double inferenceMilliseconds = 0;
    double encodingMilliseconds = 0;
    std::uint64_t workerPid = 0;
    double peakWorkerRssMiB = 0;
    std::uint64_t minimumFreeSystemRamBytes = 0;
    bool processTreeMemoryMeasured = false;
};

struct VideoResult
{
    bool succeeded = false;
    bool cancelled = false;
    std::filesystem::path path;
    std::string message;
    std::string detail;
    double elapsedMilliseconds = 0;
    VideoArtifactReceipt receipt;
    VideoArtifactExpectation expectedArtifact;
};

[[nodiscard]] bool VerifyVideoArtifact(const VideoArtifactExpectation& expected, const VideoArtifactReceipt& receipt,
    const attachments::VideoDecoderSettings& decoderSettings, std::stop_token stopToken, const std::function<bool()>& admission,
    std::string& outError);
}
