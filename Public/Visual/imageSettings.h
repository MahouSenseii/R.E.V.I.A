#pragma once

#include <string>

// Optional local Python/model worker; disabled by default.
struct imageSettings
{
    bool bEnabled = false;
    std::string pythonExecutable = "ThirdParty/ImageGen/.venv/Scripts/python.exe";
    std::string serviceScript = "Tools/revia_image_service.py";
    std::string cacheDirectory = "ThirdParty/ImageGen/cache";
    std::string outputPath = "RuntimeData/Images";
    std::string logDirectory;
    std::string host = "127.0.0.1";
    int port = 8093;
    std::string model = "stabilityai/sd-turbo";
    std::string variant;
    // "auto", "cpu", or "cuda:N". The planner resolves auto against real free VRAM.
    std::string device = "auto";
    // Below this much free video memory the worker chooses CPU. Loading onto a card the
    // chat model has already filled does not fail cleanly; it thrashes or dies mid-step.
    int minimumFreeVramMiB = 4200;
    int gpuReserveMiB = 1536;
    int cpuThreads = 4;
    // sd-turbo produces an image in a handful of steps. On CPU that is the difference
    // between under a minute and several.
    int steps = 4;
    float guidance = 0.0f;
    int width = 512;
    int height = 512;
    int startupTimeoutSeconds = 120;
    // Generation on CPU is slow rather than broken, so the ceiling is generous.
    int requestTimeoutSeconds = 900;
    bool bShutdownOnExit = true;
    bool bKeepLoaded = false;
    bool bOffline = true;
    int seed = -1;
};
