#pragma once

#include <string>
#include <vector>

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
    // One persistent worker per planned device; qwenDevice retains single-worker compatibility.
    std::vector<std::string> qwenDevices = {"auto"};
    int qwenMaxWorkers = 2;
    int qwenPrefetchFragments = 3;
    // Short replies use the fastest worker; bounded long-reply phrases retain playback order.
    int qwenFirstPhraseCharacters = 28;
    int qwenPhraseCharacters = 64;
    bool bQwenParallelLongReplies = false;
    // Use the direct predictor loop to reduce phrase latency.
    // Unsupported workers report the failure and fall back to stock generation.
    bool bQwenLowLatencyPhrase = true;
    // Requires the low-latency path; capture once per model load.
    // Reported capture failures fall back to the eager direct loop.
    bool bQwenCudaGraph = true;
    // Requires bQwenCudaGraph; capture failures preserve the predictor path.
    // Prefill remains eager; Hugging Face retains sampling, end detection and token limits.
    bool bQwenTalkerGraph = true;
    // Batch only phrases after the first, preserving first-audio latency.
    bool bQwenBatchReplyPhrases = true;
    // Phrase and character caps bound batch delay and audio-generation memory.
    int qwenMaxBatchPhrases = 6;
    int qwenMaxBatchCharacters = 480;
    bool bQwenDirectPcm = true;
    bool bQwenPrecomputeVoicePrompt = true;
    std::string qwenAttentionBackend = "adaptive";
    std::string qwenInputMode = "simulated-stream";
    int qwenMaxBufferedAudioMiB = 128;
    int qwenMinimumFreeVramMiB = 4600;
    // Effective host-thread cap applied inside the PyTorch worker. The resource planner
    // fills this even for CUDA because model preparation and audio encoding use CPU work.
    int qwenCpuThreads = 2;
    std::string qwenVoiceDesignModel = "Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign";
    std::string qwenCloneModel = "Qwen/Qwen3-TTS-12Hz-0.6B-Base";
    std::string voiceDataPath = "RuntimeData/Voices";
    int volume = 90;
    int rate = 1;
    int maxCharacters = 1400;
    // Queue capacity counts sentences; overflow drops the oldest unsaid sentence.
    int maxQueuedUtterances = 16;
};
