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
    // Anything the model produced inside <think> tags, removed from the reply itself.
    // Reasoning is not an answer: speaking it or showing it inline would be wrong, but
    // discarding it hides what Revia actually did.
    std::string reasoning;
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
    // Sized for sentences, not whole replies. Streaming hands the worker one utterance
    // per sentence, and generation easily outruns realtime playback, so a small cap here
    // silently drops the OLDEST unsaid sentence -- you would hear a reply start midway.
    int maxQueuedUtterances = 16;
};

struct alwaysOnListeningSettings
{
    // Opt-in. Continuous listening is a standing microphone, which is a materially larger
    // promise than a button that opens one for a few seconds.
    bool bEnabled = false;
    int energyThreshold = 1400;
    // Sustained speech before capture starts, so a door does not open a recording.
    int onsetFramesRequired = 5;
    // Silence before capture ends. Long enough to survive a pause mid-sentence.
    int silenceMsToEnd = 900;
    // Hard ceiling on one utterance, so a noisy room cannot record indefinitely.
    int maxUtteranceSeconds = 30;
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
    alwaysOnListeningSettings alwaysOn;
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

// Where a reply is going. Revia speaks when she is talking to the person in front of her;
// text she is composing into someone else's application is not something to read aloud.
enum class outputChannel
{
    LocalVoice,
    ExternalApplication
};

struct conversationChannelSettings
{
    // Executables Revia may speak for even when composing into them. Empty by default:
    // typing into Discord or a browser is text, and narrating it is noise.
    std::vector<std::string> voiceEnabledApplications;
    // Named so the reason a reply was silent can be reported rather than guessed at.
    std::vector<std::string> textOnlyApplications = {
        "discord.exe", "slack.exe", "teams.exe", "telegram.exe", "whatsapp.exe",
        "msedge.exe", "chrome.exe", "firefox.exe", "thunderbird.exe", "outlook.exe"
    };
};

// Merging and filtering what arrives, instead of answering every fragment separately.
struct inputArbiterSettings
{
    // Inputs landing inside this window are treated as one thought. Speaking in three
    // bursts should not produce three replies.
    int mergeWindowMs = 1500;
    // Below this, a fragment is treated as noise unless it is clearly addressed to Revia.
    int minimumMeaningfulCharacters = 3;
    int maxQueuedInputs = 8;
    // Recognisers emit these constantly from room noise. They are dropped rather than
    // answered.
    std::vector<std::string> ignoredFragments = {
        "uh", "um", "erm", "hmm", "mm", "mhm", "ah", "oh", "eh", "huh",
        "you", "thanks for watching", "thank you", "bye", "[blank_audio]", "..."
    };
};

// Stage 6's attention model and Stage 7's proposal rate, together: when Revia is allowed
// to speak first, and how quickly it must back off when it turns out to be wrong.
//
// Silence is the default. A proposal has to clear a confidence threshold, not a relevance
// one -- "this might be related" is not a reason to interrupt someone.
struct initiativeSettings
{
    bool bEnabled = false;
    // Below this, Revia stays quiet no matter how relevant the observation looks.
    float minimumConfidence = 0.72f;
    int maxUtterancesPerHour = 4;
    int cooldownSeconds = 900;
    // Longer after a dismissal than after an accepted one. Being told "no" should cost
    // more than being ignored.
    int dismissalCooldownSeconds = 3600;
    // Do not interrupt someone mid-keystroke. Measured from the last input event of any
    // kind, which needs no keyboard hook and records nothing about what was typed.
    int quietInputSeconds = 4;
    // Proposals accepted versus dismissed. Below this, Revia halves its own rate. An
    // assistant that cannot tell it is being annoying is a defect.
    float minimumPrecision = 0.34f;
    int precisionSampleFloor = 5;
    bool bSuppressWhenFullScreen = true;
};

// Stopping mid-sentence when the user starts talking, the way a person does.
//
// The hard part is that the microphone hears the speakers for the whole utterance, not
// just the start of it. A fixed threshold therefore cannot separate "the user is talking"
// from "Revia is talking and the room is echoing it back" -- which is why detection here
// tracks a rolling noise floor and looks for a step above it, rather than an absolute
// level. The floor is learned from the frames that did not trigger, so Revia's own voice
// raises the bar instead of tripping it.
struct bargeInSettings
{
    bool bEnabled = true;
    // Absolute floor. Nothing below this is ever an interruption regardless of how quiet
    // the room is, so a silent microphone cannot produce a hair trigger.
    int energyThreshold = 1400;
    // How far above the learned floor a frame must sit to count. Speech arrives on top of
    // the echo, so a genuine interruption is a step change, not a slow drift.
    float echoMarginMultiplier = 2.6f;
    // Consecutive qualifying frames before Revia yields, so one cough, a door, or a burst
    // of laughter from the speakers does not cut a reply short. Frames are ~50 ms.
    int consecutiveFramesRequired = 8;
    // Time to learn the floor before any interruption is possible. Must be long enough to
    // capture what Revia's own playback sounds like through the microphone.
    int startupGraceMs = 700;
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
    initiativeSettings initiative;
    bargeInSettings bargeIn;
    conversationChannelSettings channels;
    inputArbiterSettings inputArbiter;
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
