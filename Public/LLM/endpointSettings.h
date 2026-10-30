#pragma once

#include "LLM/Cloud/cloudSettings.h"

#include <string>

struct llmSettings
{
    std::string backend = "LLamaCpp";
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string modelName = "local-model";
    std::string apiKey;
    // Set for a server Revia starts itself. Its health check then requires the server
    // to refuse a wrong key and accept apiKey, so a process already listening on the port
    // that does not hold this session's key is never sent a conversation. Off for a
    // server the user runs (autoStartServer false) and for test fixtures.
    bool bVerifyServerKey = false;

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
    // K and V cache element type passed to llama-server, for example "q8_0". Empty keeps
    // llama.cpp's f16 default. A quantized V cache requires flash attention, so a
    // quantized type also switches flash attention from "auto" to "on".
    std::string kvCacheType;
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
    // Extra output tokens a reply may spend thinking when native thinking is on, so the
    // thinking does not eat the answer's own maxTokens budget. Capped at a quarter of
    // the context.
    int thinkingTokens = 3072;
    // Places per-turn state in the newest user message to preserve the cached system prefix.
    bool bStablePromptPrefix = true;
    bool bAllowPromptCache = true;
    // Optional cloud model, off by default. Only Main's copy is read.
    revia::llm::cloud::CloudModelSettings cloud;
    // Zero leaves image sizing to the backend; positive values bound its per-image minimum.
    int imageMinTokens = 0;
};

struct embeddingSettings
{
    bool bEnabled = true;
    std::string host = "127.0.0.1";
    int port = 8081;
    std::string modelName = "nomic-embed-text-v1.5.Q4_K_M.gguf";
    std::string apiKey;
    // As llmSettings::bVerifyServerKey, for the embedding server.
    bool bVerifyServerKey = false;

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
