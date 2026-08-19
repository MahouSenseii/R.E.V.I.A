#pragma once

#include <string>
#include <vector>

#include "enumLibrary.h"

struct latencySample
{
    std::string stage;
    double milliseconds = 0.0;
    bool bAggregate = false;
};

struct responseOutput
{
    bool bSuccess = false;
    bool bShouldRemember = false;
    bool bShouldSpeak = true;
    bool bWasStreamed = false;  // true if tokens were already printed during generation

    std::string response;
    std::string reason;
    std::vector<latencySample> timings;
};

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
    int autoFitTargetMiB = 1024;
    // Set at runtime, not from settings.json. VRAM that llama.cpp must leave free on top
    // of autoFitTargetMiB because another local model still has to load into it. The
    // Qwen3-TTS service chooses CPU over CUDA when free VRAM is below its own threshold,
    // so without this reservation a voice loaded after llama.cpp would land on the CPU.
    int reservedVramMiB = 0;
    int contextSize = 4096;
    int parallelRequests = 2;
    int startupTimeoutSeconds = 120;
    bool bShutdownServerOnExit = true;
    bool bVisionEnabled = true;
    std::string multimodalProjectorPath =
        "Models/Qwen3-VL-8B-Instruct-Unredacted-MAX.mmproj-q8_0.gguf";
    std::string mediaPath = "RuntimeData/Vision";

    float temperature = 0.7f;
    bool bAutoMaxTokens = true;
    int maxTokens = 4096;
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
    std::string queryPrefix = "search_query: ";
    std::string documentPrefix = "search_document: ";
};

struct speechSettings
{
    bool bEnabled = true;
    bool bSpeakGreeting = false;
    std::string backend = "Auto";
    std::string pythonExecutable = "python";
    std::string qwenServiceScript = "Tools/qwen_tts_service.py";
    std::string qwenHost = "127.0.0.1";
    int qwenPort = 8092;
    int qwenStartupTimeoutSeconds = 60;
    int qwenRequestTimeoutSeconds = 600;
    std::string qwenDevice = "auto";
    int qwenMinimumFreeVramMiB = 4600;
    std::string qwenVoiceDesignModel = "Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign";
    std::string qwenCloneModel = "Qwen/Qwen3-TTS-12Hz-0.6B-Base";
    std::string voiceDataPath = "RuntimeData/Voices";
    int volume = 90;
    int rate = 1;
    int maxCharacters = 1400;
    int maxQueuedUtterances = 2;
};

struct speechRecognitionSettings
{
    bool bEnabled = true;
    std::string executable = "ThirdParty/whisper/whisper-cli.exe";
    std::string modelPath = "Models/ggml-small.en.bin";
    std::string language = "en";
    int sampleRate = 16000;
    int threads = 6;
    bool bUseGpu = true;
};

struct visionSettings
{
    bool bEnabled = true;
    bool bRequireConfirmation = true;
    int maxResponseTokens = 768;
};

// Stage 6 Tier 0. Window and focus events only: which application is in front and what
// its title says. No capture, no pixels, no model.
//
// Continuous observation is the most invasive capability in this project, so it is off
// until asked for, and the exclusion lists deny by default rather than allow by default.
// An application or title that matches is not recorded in redacted form -- it produces no
// observation at all, because "the user switched to their bank at 14:02" is the leak.
struct perceptionSettings
{
    bool bEnabled = false;
    // Coalescing window. Title changes fire per keystroke in some editors, and a
    // per-keystroke record of a document title is a transcript by another name.
    int minimumEventIntervalMs = 750;
    int maxObservationsPerMinute = 60;
    std::vector<std::string> excludedApplications = {
        "keepass.exe", "keepassxc.exe", "1password.exe", "bitwarden.exe",
        "lastpass.exe", "dashlane.exe", "protonpass.exe", "enpass.exe"
    };
    std::vector<std::string> excludedTitleFragments = {
        "incognito", "inprivate", "private browsing", "private window",
        "password", "passphrase", "seed phrase", "recovery phrase",
        "authenticator", "one-time code", "bank", "banking", "credit card"
    };
};

struct aiProfile
{
    std::string id = "assistant";
    std::string displayName = "Assistant";
    std::string systemPrompt = "You are a helpful local AI assistant.";

    // H3: when false, user input is not written to long-term memory.
    bool bMemoryEnabled = true;

    bool bHasTemperatureOverride = false;
    bool bHasMaxTokensOverride = false;

    float temperature = 0.7f;
    int maxTokens = 512;
};

struct appSettings
{
    std::string activeProfile = "assistant";
    llmSettings llm;
    embeddingSettings embedding;
    speechSettings speech;
    speechRecognitionSettings speechRecognition;
    visionSettings vision;
    perceptionSettings perception;
};

struct commandOutput
{
    bool bWasCommand = false;
    bool bShouldExit = false;
    bool bSuccess = true;

    std::string output;
    std::string reason;
};

struct conversationMessage
{
    std::string role;
    std::string content;
};

struct healthOutput
{
    bool bIsAvailable = false;

    systemStatus status = systemStatus::Red;

    std::string name;
    std::string message;
    std::string reason;
    int contextTokens = 0;
    int parallelSlots = 0;
    int responseTokenLimit = 0;
};

struct memoryEntry
{
    std::string id;
    std::string category;
    std::string summary;
    std::string source;
    std::string createdAt;

    memoryImportance importance = memoryImportance::Medium;
};

struct memoryDecision
{
    bool bSuccess = false;
    bool bShouldRemember = false;

    std::string category;
    std::string summary;
    std::string reason;
    std::vector<float> embedding;
    std::string embeddingModel;
    std::vector<latencySample> timings;
};

struct embeddingOutput
{
    bool bSuccess = false;
    std::vector<float> values;
    std::string model;
    std::string reason;
    double elapsedMilliseconds = 0.0;
};
