#pragma once

#include <string>

struct llmSettings
{
    std::string backend = "LLamaCpp";
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string modelName = "local-model";
    std::string apiKey;

    bool bAutoStartServer = false;
    std::string serverExecutable;
    std::string modelPath;
    bool bAutoTune = true;
    // Reserve VRAM for normal desktop GPU usage after startup.
    int autoFitTargetMiB = 2048;
    // Runtime-only VRAM reservation for other models, added to autoFitTargetMiB.
    int reservedVramMiB = 0;
    // Startup placement: "auto" delegates to llama.cpp; comma-separated IDs pin devices.
    std::string device = "auto";
    std::string splitMode = "none";
    std::string tensorSplit;
    std::string fitTargetMiB;
    int cpuThreads = 0;
    int cpuBatchThreads = 0;
    // -1 leaves the llama.cpp default alone, 0 disables its prompt cache, and a
    // positive value is the real maximum RAM allocation passed to --cache-ram.
    int ramCacheMiB = -1;
    std::string modelLoadMode = "auto";
    int contextSize = 8192;
    int parallelRequests = 2;
    int startupTimeoutSeconds = 120;
    bool bShutdownServerOnExit = true;
    bool bVisionEnabled = true;
    std::string multimodalProjectorPath =
        "Models/Qwen3-VL-8B-Instruct-Unredacted-MAX.mmproj-q8_0.gguf";
    std::string mediaPath = "RuntimeData/Vision";
    std::string logDirectory;

    float temperature = 0.7f;
    bool bAutoMaxTokens = true;
    int maxTokens = 4096;
    // Places per-turn state in the newest user message to preserve the cached system prefix.
    bool bStablePromptPrefix = true;
    bool bAllowPromptCache = true;
};

struct embeddingSettings
{
    bool bEnabled = true;
    std::string host = "127.0.0.1";
    int port = 8081;
    std::string modelName = "nomic-embed-text-v1.5.Q4_K_M.gguf";
    std::string apiKey;

    bool bAutoStartServer = true;
    std::string serverExecutable;
    std::string modelPath;
    int contextSize = 2048;
    int parallelRequests = 2;
    int startupTimeoutSeconds = 60;
    bool bShutdownServerOnExit = true;
    std::string pooling = "mean";
    std::string device = "none";
    int cpuThreads = 0;
    int cpuBatchThreads = 0;
    int ramCacheMiB = 0;
    std::string modelLoadMode = "mmap";
    std::string queryPrefix = "search_query: ";
    std::string documentPrefix = "search_document: ";
    std::string logDirectory;
};
