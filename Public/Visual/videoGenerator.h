#pragma once

#include "Visual/videoSettings.h"
#include "Visual/videoTypes.h"

#include <atomic>
#include <mutex>

namespace revia::visual
{
struct VideoJobSnapshot
{
    std::string state = "idle";
    std::string jobId;
    std::string detail;
    int step = 0;
    int steps = 50;
};

class VideoGenerator
{
  public:
    ~VideoGenerator();
    void Configure(videoSettings settings);
    [[nodiscard]] bool IsEnabled() const;
    [[nodiscard]] bool IsAvailable(std::string& outDetail);
    [[nodiscard]] std::filesystem::path OutputDirectory();
    VideoResult Generate(const VideoRequest& request, std::stop_token stopToken = {}, std::function<bool()> admission = {},
        const std::filesystem::path& expectedOutputRoot = {});
    void Cancel();
    [[nodiscard]] VideoJobSnapshot Snapshot() const;
    void Shutdown();

  private:
    void SetSnapshot(VideoJobSnapshot snapshot);
    std::mutex mutex;
    mutable std::mutex statusMutex;
    videoSettings configuration;
    VideoJobSnapshot snapshot;
    std::stop_source activeStop;
    std::atomic<std::uint64_t> cancellationGeneration = 0;
    std::atomic<bool> enabled = false;
    std::atomic<bool> shuttingDown = false;
};
}
