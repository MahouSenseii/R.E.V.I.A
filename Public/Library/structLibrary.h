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
    bool bWasStreamed = false;  // true when visible deltas were delivered to a consumer

    std::string response;
    // What the model actually produced, before the deterministic style repair ran. The
    // delivered reply is what the user experiences and what quality is scored on, but a
    // regression suite has to be able to tell "the model is still good" apart from "the
    // repair layer caught it again", because only one of those keeps working.
    std::string rawResponse;
    std::string reason;
    // Anything the model produced inside <think> tags, removed from the reply itself.
    // Reasoning is not an answer: speaking it or showing it inline would be wrong, but
    // discarding it hides what Revia actually did.
    std::string reasoning;
    // The hard layer is deterministic and always runs. The optional AI review is a
    // second, independent pass over the completed candidate before it can reach speech,
    // memory, or dialogue history. These fields are observability, not hidden policy.
    bool bHardFilterChanged = false;
    bool bAiFilterReviewed = false;
    bool bAiFilterChanged = false;
    std::string filterSummary;
    // Pre-generation routing telemetry. Strings keep this transport structure independent
    // of the router implementation while still making every fallback auditable.
    std::string requestedTier;
    std::string selectedTier;
    std::string selectedModel;
    std::string reasoningMode;
    std::string routingReason;
    float routingConfidence = 0.0F;
    bool bRoutingFallback = false;
    std::string routingFallbackReason;
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
    // Leave enough room for normal desktop GPU use to grow after startup. A one-GiB
    // reserve proved too narrow on an 8-GiB laptop once the compositor and UI changed.
    int autoFitTargetMiB = 2048;
    // Set at runtime, not from settings.json. VRAM that llama.cpp must leave free on top
    // of autoFitTargetMiB because another local model still has to load into it. The
    // Qwen3-TTS service chooses CPU over CUDA when free VRAM is below its own threshold,
    // so without this reservation a voice loaded after llama.cpp would land on the CPU.
    int reservedVramMiB = 0;
    // Filled by the resource planner at startup. "auto" preserves llama.cpp's own
    // backend choice; an explicit comma-separated list pins this process to those
    // devices. Keeping placement out of the process owner makes the launch path usable
    // by both automatic and manual plans.
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

// The three conversational roles share one profile, prompt builder, memory store, and
// humanization state. These settings describe only the extra model endpoints; hardware
// placement is resolved at runtime so a one-GPU laptop never inherits workstation GPU
// ordinals.
struct modelTierSettings
{
    bool bEnabled = false;
    std::string host = "127.0.0.1";
    int port = 0;
    std::string modelName;
    std::string modelPath;
    bool bVisionEnabled = false;
    std::string multimodalProjectorPath;
    int contextSize = 4096;
    int maxTokens = 384;
    float temperature = 0.75F;
    int startupTimeoutSeconds = 120;
    bool bWarmAtStartup = true;
};

struct intelligenceSettings
{
    bool bEnabled = true;
    modelTierSettings fast = {
        true,
        "127.0.0.1",
        8082,
        "Qwen3.5-0.8B-Q4_K_M.gguf",
        "Models/Qwen3.5-0.8B-Q4_K_M.gguf",
        false,
        "",
        4096,
        256,
        0.78F,
        90,
        true};
    modelTierSettings expert = {
        true,
        "127.0.0.1",
        8083,
        "Qwen3-VL-8B-Instruct-Unredacted-MAX.Q4_K_M.gguf",
        "Models/Qwen3-VL-8B-Instruct-Unredacted-MAX.Q4_K_M.gguf",
        true,
        "Models/Qwen3-VL-8B-Instruct-Unredacted-MAX.mmproj-q8_0.gguf",
        4096,
        1024,
        0.72F,
        180,
        true};
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
    // One long-lived Qwen process is created per entry. The resource planner replaces
    // the default with the exact devices that can hold an independent clone model.
    // Keeping qwenDevice preserves older configurations and the single-worker UI path.
    std::vector<std::string> qwenDevices = {"auto"};
    int qwenMaxWorkers = 2;
    int qwenPrefetchFragments = 3;
    // Short replies stay on the fastest worker. Longer replies are split into bounded
    // phrases, allowing additional local workers to synthesize ahead while playback
    // remains sequence-ordered.
    int qwenPhraseCharacters = 96;
    bool bQwenParallelLongReplies = false;
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
    // Sized for sentences, not whole replies. Streaming hands the worker one utterance
    // per sentence, and generation easily outruns realtime playback, so a small cap here
    // silently drops the OLDEST unsaid sentence -- you would hear a reply start midway.
    int maxQueuedUtterances = 16;
};

struct speechRecognitionSettings
{
    bool bEnabled = true;
    std::string executable = "ThirdParty/whisper/whisper-cli.exe";
    bool bUseServer = true;
    std::string serverExecutable = "ThirdParty/whisper/whisper-server.exe";
    std::string serverHost = "127.0.0.1";
    int serverPort = 8094;
    int serverStartupTimeoutSeconds = 60;
    int requestTimeoutSeconds = 180;
    std::string modelPath = "Models/ggml-small.en.bin";
    std::string language = "en";
    int sampleRate = 16000;
    int threads = 6;
    bool bUseGpu = true;
    // "cpu", "auto", or "cuda:N". `useGpu` remains as the backward-compatible
    // coarse switch; the startup resource plan resolves auto to one exact device.
    std::string device = "auto";
    // Hands-free mode records only voiced segments. It is deliberately a local comfort
    // setting rather than an authority setting: transcripts still enter the same input
    // arbiter and cannot widen action permissions.
    bool bHandsFree = false;
    int vadEnergyThreshold = 900;
    int vadSpeechFrames = 3;
    int vadSilenceMs = 350;
    int minimumUtteranceMs = 350;
    int maximumUtteranceSeconds = 24;
};

// Presence is a presentation and input-routing layer. It never owns inference, memory,
// actions, or rendering. A VRM renderer and narrow external integrations consume its
// bounded local files and can disappear without taking down the assistant.
struct presenceSettings
{
    bool bEnabled = true;
    bool bAvatarBridgeEnabled = true;
    std::string statePath = "RuntimeData/Presence/avatar_state.json";
    std::string eventPath = "RuntimeData/Presence/avatar_events.jsonl";
    bool bExternalAdaptersEnabled = false;
    std::string inboxPath = "RuntimeData/Presence/Inbox";
    std::string outboxPath = "RuntimeData/Presence/Outbox";
    int adapterPollMs = 150;
    int maxAdapterEventsPerMinute = 30;
    int maxAdapterTextCharacters = 4000;
    std::vector<std::string> allowedAdapters = {"discord", "stream", "game"};
};

// Cross-pipeline placement policy. These are budgets and preferences, not work queues:
// each service still owns its process/thread lifecycle and the planner only decides what
// resources that owner is allowed to consume.
struct resourceSettings
{
    bool bAutoPlan = true;
    int reserveLogicalCores = 2;
    int minimumFreeRamMiB = 4096;
    // 0 derives a bounded value from total and currently available RAM.
    int llamaPromptCacheMiB = 0;
    // Combined SQLite page+mmap ceiling per connection. Zero derives 1/256 of system
    // RAM, capped at 512 MiB; this is a ceiling, not a preallocation.
    int sqliteCacheMiB = 0;
    int gpuReserveMiB = 1536;
    // How often live usage is sampled against the plan. Zero turns the monitor off; the
    // plan itself is unaffected either way, because observing never re-places a worker.
    int usageSampleSeconds = 2;
    bool bAllowChatModelSplit = false;
    std::string chat = "auto-primary";
    std::string voice = "auto-secondary";
    std::string speechRecognition = "auto-secondary";
    std::string embeddings = "cpu";
};

struct visionSettings
{
    bool bEnabled = true;
    bool bRequireConfirmation = true;
    int maxResponseTokens = 768;
    // Event-driven continuous awareness is separate from action authority. It may keep a
    // short local description of the virtual desktop, but it can never click or type.
    bool bContinuousAwareness = false;
    int awarenessDebounceMs = 1500;
    int awarenessMinimumIntervalMs = 6000;
    int awarenessMaxResponseTokens = 160;
    double resolutionConfidence = 0.72;
    double minimumNameAgreement = 0.35;
    double ambiguityMargin = 0.08;
    int maxResolverElements = 500;
};

// Stage 6 Tier 0 records window and focus events. When vision's separately configured
// continuous-awareness layer is enabled, accepted events may also trigger a temporary
// local virtual-desktop capture whose bounded summary is kept in memory.
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
    int mergeWindowMs = 350;
    // Below this, a fragment is treated as noise unless it is clearly addressed to Revia.
    int minimumMeaningfulCharacters = 3;
    int maxQueuedInputs = 8;
    // Recognisers emit these constantly from room noise. They are dropped rather than
    // answered.
    std::vector<std::string> ignoredFragments = {
        "uh", "um", "erm", "hmm", "mm", "mhm", "ah", "oh", "eh", "huh",
        "you", "thanks for watching", "thank you", "[blank_audio]", "..."
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
    // Curiosity is a candidate generator, not an interruption permission. It may
    // consider recent dialogue or a meaningful affect transition, but an empty clock
    // tick can never create a topic and AttentionPolicy still owns the final gate.
    bool bCuriosityEnabled = true;
    bool bSpontaneousSpeechEnabled = true;
    bool bSpeakWhenUserAway = true;
    // When a permitted autonomous lookup produces a grounded spoken finding, keep a
    // bounded model-written summary plus source URLs as durable memory. Raw page bodies
    // and private reasoning are never stored.
    bool bAutonomousLearningEnabled = false;
    int curiosityCheckSeconds = 30;
    int autonomousQuietSeconds = 45;
    int curiosityTopicCooldownMinutes = 1440;
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
    // Event-pattern thresholds. Time constrains what counts as meaningful evidence; it
    // never creates an utterance by itself. A foreground transition must complete the
    // pattern and wake the initiative worker.
    int focusSessionMinutes = 12;
    int returnAfterMinutes = 20;
    int contextSwitchWindowSeconds = 300;
    int contextSwitchCount = 6;
    int cueMaxAgeMinutes = 10;
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

// Local image generation. Off by default: it is an optional Python runtime and a
// multi-gigabyte model download, and a machine without it should behave as though the
// feature simply does not exist rather than failing at the moment it is asked.
struct imageSettings
{
    bool bEnabled = false;
    std::string pythonExecutable = "ThirdParty/ImageGen/.venv/Scripts/python.exe";
    std::string serviceScript = "Tools/revia_image_service.py";
    std::string cacheDirectory = "ThirdParty/ImageGen/cache";
    std::string outputPath = "RuntimeData/Images";
    std::string host = "127.0.0.1";
    int port = 8093;
    std::string model = "stabilityai/sd-turbo";
    // "auto", "cpu", or "cuda:N". The planner resolves auto against real free VRAM.
    std::string device = "auto";
    // Below this much free video memory the worker chooses CPU. Loading onto a card the
    // chat model has already filled does not fail cleanly; it thrashes or dies mid-step.
    int minimumFreeVramMiB = 4200;
    // sd-turbo produces an image in a handful of steps. On CPU that is the difference
    // between under a minute and several.
    int steps = 4;
    float guidance = 1.0f;
    int width = 512;
    int height = 512;
    int startupTimeoutSeconds = 120;
    // Generation on CPU is slow rather than broken, so the ceiling is generous.
    int requestTimeoutSeconds = 900;
    bool bShutdownOnExit = true;
};

// Durable conversation history. A separate block from memory because it is a separate
// promise: memory keeps facts a classifier judged worth having, this keeps what was said.
struct conversationSettings
{
    bool bArchiveEnabled = true;
    // Ceilings, not targets. An archive that grows without bound becomes a liability the
    // user never agreed to keep.
    int maxSessions = 200;
    int maxTurnsPerSession = 500;
    int maxTurnCharacters = 8000;
    // How much of the previous session is replayed into context at startup, so a restart
    // continues a conversation instead of restarting one. Costs prompt tokens every turn
    // it survives, which is why it is small.
    int restoreTurns = 6;
};

// Response filtering is deliberately separate from the personality prompt. A profile
// may be playful or bratty without being trusted to police its own output. The hard
// structural/grounding pass cannot be disabled; the model review can be traded for
// lower latency and is exposed as a live comfort preference.
struct responseFilterSettings
{
    bool bAiReviewEnabled = false;
    int aiMaxReviewTokens = 192;
    int maxReplyCharacters = 12000;
};

struct appSettings
{
    std::string activeProfile = "assistant";
    llmSettings llm;
    intelligenceSettings intelligence;
    embeddingSettings embedding;
    speechSettings speech;
    speechRecognitionSettings speechRecognition;
    presenceSettings presence;
    resourceSettings resources;
    visionSettings vision;
    perceptionSettings perception;
    initiativeSettings initiative;
    bargeInSettings bargeIn;
    conversationChannelSettings channels;
    conversationSettings conversation;
    responseFilterSettings responseFilter;
    imageSettings image;
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
    // Identifies how a preclassified memory entered the store. Ordinary conversation
    // remains "automatic"; sourced background findings use "autonomous_research".
    std::string source = "automatic";
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
