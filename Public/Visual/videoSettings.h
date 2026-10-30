#pragma once

#include <string>

struct videoSettings
{
    bool bEnabled = false;
    std::string pythonExecutable = "ThirdParty/ImageGen/.venv/Scripts/python.exe";
    std::string workerScript = "Tools/revia_video_worker.py";
    std::string modelDirectory;
    std::string runtimeOverlayDirectory;
    std::string outputPath = "RuntimeData/Videos";
    std::string ffmpegPath = "ThirdParty/ffmpeg/bin/ffmpeg.exe";
    std::string ffprobePath = "ThirdParty/ffmpeg/bin/ffprobe.exe";
    int requestTimeoutSeconds = 600;
    int width = 384;
    int height = 256;
    int frameCount = 49;
    int fps = 25;
    int steps = 50;
    int gpuIndex = 1;
    int cpuThreads = 6;
};
