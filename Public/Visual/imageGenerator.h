#pragma once


#include "Visual/imageSettings.h"
#include "Visual/imageTypes.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <stop_token>

namespace revia::visual
{

struct ImageJobSnapshot
{
    std::string state = "idle";
    std::string jobId;
    std::string model;
    std::string device;
    std::string detail;
    int step = 0;
    int steps = 0;
    bool loaded = false;
};

// Owns the local image worker process. Same shape as QwenTtsServerProcess and for the
// same reasons: the model runtime is Python, the lifecycle is C++, and the child lives
// in a kill-on-close job so it cannot outlive Revia.
class ImageServerProcess
{
public:
    ImageServerProcess() = default;
    ~ImageServerProcess();

    ImageServerProcess(const ImageServerProcess&) = delete;
    ImageServerProcess& operator=(const ImageServerProcess&) = delete;

    bool Start(const imageSettings& settings, const std::string& apiKey, std::string& outError);
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] bool WasStartedByRevia() const;
    void Stop();

private:
#ifdef _WIN32
    void* processHandle = nullptr;
    void* jobHandle = nullptr;
#endif
    bool bShutdownOnExit = true;
};

// Generates pictures through the local worker.
//
// Deliberately distinct from the diagram path. An LLM emitting SVG draws boxes and
// arrows and cannot draw a picture; this draws a picture and cannot lay out an
// interface. Presenting them as one capability would mean one of the two is always the
// wrong tool and the user has no way to ask for the other.
class ImageGenerator
{
public:
    ImageGenerator() = default;
    ~ImageGenerator();

    ImageGenerator(const ImageGenerator&) = delete;
    ImageGenerator& operator=(const ImageGenerator&) = delete;

    void Configure(imageSettings settings);
    [[nodiscard]] bool IsEnabled() const;
    [[nodiscard]] std::filesystem::path OutputDirectory();
    // Reports what is missing rather than a bare false, because the usual answer is "the
    // Python runtime was never installed" and that is fixable in one command.
    [[nodiscard]] bool IsAvailable(std::string& outDetail);
    ImageResult Generate(const std::string& prompt, const std::string& negativePrompt = {}, std::stop_token stopToken = {},
        std::function<bool()> admission = {}, const std::filesystem::path& expectedOutputRoot = {});
    void Cancel();
    void Unload();
    [[nodiscard]] ImageJobSnapshot Snapshot() const;
    void Shutdown();

private:
  bool EnsureRunning(std::string& outError, const std::function<bool()>& cancelled);
  void SetSnapshot(ImageJobSnapshot value);

  std::mutex mutex;
  mutable std::mutex statusMutex;
  ImageJobSnapshot status;
  std::atomic<bool> enabled = false;
  std::atomic<unsigned long long> cancellationGeneration = 0;
  std::atomic<bool> shuttingDown = false;
  imageSettings configuration;
  std::string apiKey;
  ImageServerProcess process;
};

} // namespace revia::visual
