#include "Core/configManager.h"

#include <exception>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>

#include "Library/structLibrary.h"

using json = nlohmann::json;

namespace
{
    bool IsDeviceSelector(const std::string& value, const bool cudaOnly)
    {
        static const std::regex Common(
            R"(^(auto|auto-primary|auto-secondary|cpu|none)$)");
        static const std::regex Cuda(
            R"(^cuda:[0-9]+$)",
            std::regex_constants::icase);
        static const std::regex Backend(
            R"(^[a-z][a-z0-9_-]*[0-9]+$)",
            std::regex_constants::icase);
        return std::regex_match(value, Common) || std::regex_match(value, Cuda) ||
            (!cudaOnly && std::regex_match(value, Backend));
    }

    void ReadModelTier(const json& data, modelTierSettings& output)
    {
        if (data.contains("enabled")) output.bEnabled = data["enabled"].get<bool>();
        if (data.contains("host")) output.host = data["host"].get<std::string>();
        if (data.contains("port")) output.port = data["port"].get<int>();
        if (data.contains("modelName"))
            output.modelName = data["modelName"].get<std::string>();
        if (data.contains("modelPath"))
            output.modelPath = data["modelPath"].get<std::string>();
        if (data.contains("visionEnabled"))
            output.bVisionEnabled = data["visionEnabled"].get<bool>();
        if (data.contains("multimodalProjectorPath"))
            output.multimodalProjectorPath =
                data["multimodalProjectorPath"].get<std::string>();
        if (data.contains("contextSize"))
            output.contextSize = data["contextSize"].get<int>();
        if (data.contains("maxTokens"))
            output.maxTokens = data["maxTokens"].get<int>();
        if (data.contains("temperature"))
            output.temperature = data["temperature"].get<float>();
        if (data.contains("startupTimeoutSeconds"))
            output.startupTimeoutSeconds = data["startupTimeoutSeconds"].get<int>();
        if (data.contains("warmAtStartup"))
            output.bWarmAtStartup = data["warmAtStartup"].get<bool>();
    }

    bool IsValidModelTier(const modelTierSettings& tier)
    {
        if (!tier.bEnabled) return true;
        return tier.host == "127.0.0.1" && tier.port > 0 && tier.port <= 65535 &&
            !tier.modelName.empty() && !tier.modelPath.empty() &&
            tier.contextSize >= 512 && tier.contextSize <= 1048576 &&
            tier.maxTokens >= 1 && tier.maxTokens <= 32768 &&
            tier.temperature >= 0.0F && tier.temperature <= 2.0F &&
            tier.startupTimeoutSeconds >= 1 && tier.startupTimeoutSeconds <= 600 &&
            (!tier.bVisionEnabled || !tier.multimodalProjectorPath.empty());
    }
}

configManager::configManager() = default;

configManager::~configManager() = default;

bool configManager::LoadSettings(appSettings& outSettings) const
{
    std::ifstream file(settingsPath);

    if (!file.is_open())
    {
        return false;
    }

    try
    {
        json data;
        file >> data;

        if (data.contains("activeProfile"))
        {
            outSettings.activeProfile = data["activeProfile"].get<std::string>();
        }

        if (data.contains("llm"))
        {
            const json& llmData = data["llm"];

            if (llmData.contains("backend"))
            {
                outSettings.llm.backend = llmData["backend"].get<std::string>();
            }

            if (llmData.contains("host"))
            {
                outSettings.llm.host = llmData["host"].get<std::string>();
            }

            if (llmData.contains("port"))
            {
                outSettings.llm.port = llmData["port"].get<int>();
            }

            if (llmData.contains("modelName"))
            {
                outSettings.llm.modelName = llmData["modelName"].get<std::string>();
            }
            if (llmData.contains("apiKey"))
            {
                outSettings.llm.apiKey = llmData["apiKey"].get<std::string>();
            }

            if (llmData.contains("autoStartServer"))
            {
                outSettings.llm.bAutoStartServer = llmData["autoStartServer"].get<bool>();
            }

            if (llmData.contains("serverExecutable"))
            {
                outSettings.llm.serverExecutable = llmData["serverExecutable"].get<std::string>();
            }

            if (llmData.contains("modelPath"))
            {
                outSettings.llm.modelPath = llmData["modelPath"].get<std::string>();
            }

            if (llmData.contains("autoTune"))
            {
                outSettings.llm.bAutoTune = llmData["autoTune"].get<bool>();
            }

            if (llmData.contains("autoFitTargetMiB"))
            {
                outSettings.llm.autoFitTargetMiB = llmData["autoFitTargetMiB"].get<int>();
            }

            if (llmData.contains("contextSize"))
            {
                outSettings.llm.contextSize = llmData["contextSize"].get<int>();
            }

            if (llmData.contains("parallelRequests"))
            {
                outSettings.llm.parallelRequests = llmData["parallelRequests"].get<int>();
            }

            if (llmData.contains("startupTimeoutSeconds"))
            {
                outSettings.llm.startupTimeoutSeconds = llmData["startupTimeoutSeconds"].get<int>();
            }

            if (llmData.contains("shutdownServerOnExit"))
            {
                outSettings.llm.bShutdownServerOnExit = llmData["shutdownServerOnExit"].get<bool>();
            }
            if (llmData.contains("visionEnabled"))
            {
                outSettings.llm.bVisionEnabled = llmData["visionEnabled"].get<bool>();
            }
            if (llmData.contains("multimodalProjectorPath"))
            {
                outSettings.llm.multimodalProjectorPath =
                    llmData["multimodalProjectorPath"].get<std::string>();
            }
            if (llmData.contains("mediaPath"))
            {
                outSettings.llm.mediaPath = llmData["mediaPath"].get<std::string>();
            }

            if (llmData.contains("temperature"))
            {
                outSettings.llm.temperature = llmData["temperature"].get<float>();
            }

            if (llmData.contains("autoMaxTokens"))
            {
                outSettings.llm.bAutoMaxTokens = llmData["autoMaxTokens"].get<bool>();
            }

            if (llmData.contains("maxTokens"))
            {
                outSettings.llm.maxTokens = llmData["maxTokens"].get<int>();
            }
        }

        if (data.contains("intelligence"))
        {
            const json& intelligenceData = data["intelligence"];
            if (intelligenceData.contains("enabled"))
            {
                outSettings.intelligence.bEnabled =
                    intelligenceData["enabled"].get<bool>();
            }
            if (intelligenceData.contains("fast"))
                ReadModelTier(intelligenceData["fast"], outSettings.intelligence.fast);
            if (intelligenceData.contains("expert"))
                ReadModelTier(intelligenceData["expert"], outSettings.intelligence.expert);
        }

        if (data.contains("embedding"))
        {
            const json& embeddingData = data["embedding"];
            if (embeddingData.contains("enabled"))
            {
                outSettings.embedding.bEnabled = embeddingData["enabled"].get<bool>();
            }
            if (embeddingData.contains("host"))
            {
                outSettings.embedding.host = embeddingData["host"].get<std::string>();
            }
            if (embeddingData.contains("port"))
            {
                outSettings.embedding.port = embeddingData["port"].get<int>();
            }
            if (embeddingData.contains("modelName"))
            {
                outSettings.embedding.modelName = embeddingData["modelName"].get<std::string>();
            }
            if (embeddingData.contains("apiKey"))
            {
                outSettings.embedding.apiKey = embeddingData["apiKey"].get<std::string>();
            }
            if (embeddingData.contains("autoStartServer"))
            {
                outSettings.embedding.bAutoStartServer = embeddingData["autoStartServer"].get<bool>();
            }
            if (embeddingData.contains("serverExecutable"))
            {
                outSettings.embedding.serverExecutable = embeddingData["serverExecutable"].get<std::string>();
            }
            if (embeddingData.contains("modelPath"))
            {
                outSettings.embedding.modelPath = embeddingData["modelPath"].get<std::string>();
            }
            if (embeddingData.contains("contextSize"))
            {
                outSettings.embedding.contextSize = embeddingData["contextSize"].get<int>();
            }
            if (embeddingData.contains("parallelRequests"))
            {
                outSettings.embedding.parallelRequests = embeddingData["parallelRequests"].get<int>();
            }
            if (embeddingData.contains("startupTimeoutSeconds"))
            {
                outSettings.embedding.startupTimeoutSeconds =
                    embeddingData["startupTimeoutSeconds"].get<int>();
            }
            if (embeddingData.contains("shutdownServerOnExit"))
            {
                outSettings.embedding.bShutdownServerOnExit =
                    embeddingData["shutdownServerOnExit"].get<bool>();
            }
            if (embeddingData.contains("pooling"))
            {
                outSettings.embedding.pooling = embeddingData["pooling"].get<std::string>();
            }
            if (embeddingData.contains("device"))
            {
                outSettings.embedding.device = embeddingData["device"].get<std::string>();
            }
            if (embeddingData.contains("queryPrefix"))
            {
                outSettings.embedding.queryPrefix = embeddingData["queryPrefix"].get<std::string>();
            }
            if (embeddingData.contains("documentPrefix"))
            {
                outSettings.embedding.documentPrefix =
                    embeddingData["documentPrefix"].get<std::string>();
            }
        }

        if (data.contains("speech"))
        {
            const json& speechData = data["speech"];
            if (speechData.contains("enabled"))
            {
                outSettings.speech.bEnabled = speechData["enabled"].get<bool>();
            }
            if (speechData.contains("speakGreeting"))
            {
                outSettings.speech.bSpeakGreeting = speechData["speakGreeting"].get<bool>();
            }
            if (speechData.contains("backend"))
            {
                outSettings.speech.backend = speechData["backend"].get<std::string>();
            }
            if (speechData.contains("pythonExecutable"))
            {
                outSettings.speech.pythonExecutable =
                    speechData["pythonExecutable"].get<std::string>();
            }
            if (speechData.contains("qwenServiceScript"))
            {
                outSettings.speech.qwenServiceScript =
                    speechData["qwenServiceScript"].get<std::string>();
            }
            if (speechData.contains("qwenHost"))
            {
                outSettings.speech.qwenHost = speechData["qwenHost"].get<std::string>();
            }
            if (speechData.contains("qwenPort"))
            {
                outSettings.speech.qwenPort = speechData["qwenPort"].get<int>();
            }
            if (speechData.contains("qwenStartupTimeoutSeconds"))
            {
                outSettings.speech.qwenStartupTimeoutSeconds =
                    speechData["qwenStartupTimeoutSeconds"].get<int>();
            }
            if (speechData.contains("qwenRequestTimeoutSeconds"))
            {
                outSettings.speech.qwenRequestTimeoutSeconds =
                    speechData["qwenRequestTimeoutSeconds"].get<int>();
            }
            if (speechData.contains("qwenDevice"))
            {
                outSettings.speech.qwenDevice = speechData["qwenDevice"].get<std::string>();
            }
            if (speechData.contains("qwenDevices") && speechData["qwenDevices"].is_array())
            {
                outSettings.speech.qwenDevices.clear();
                for (const auto& device : speechData["qwenDevices"])
                {
                    if (device.is_string())
                    {
                        outSettings.speech.qwenDevices.push_back(device.get<std::string>());
                    }
                }
            }
            if (speechData.contains("qwenMaxWorkers"))
            {
                outSettings.speech.qwenMaxWorkers = speechData["qwenMaxWorkers"].get<int>();
            }
            if (speechData.contains("qwenPrefetchFragments"))
            {
                outSettings.speech.qwenPrefetchFragments =
                    speechData["qwenPrefetchFragments"].get<int>();
            }
            if (speechData.contains("qwenPhraseCharacters"))
            {
                outSettings.speech.qwenPhraseCharacters =
                    speechData["qwenPhraseCharacters"].get<int>();
            }
            if (speechData.contains("qwenParallelLongReplies"))
            {
                outSettings.speech.bQwenParallelLongReplies =
                    speechData["qwenParallelLongReplies"].get<bool>();
            }
            if (speechData.contains("qwenMaxBufferedAudioMiB"))
            {
                outSettings.speech.qwenMaxBufferedAudioMiB =
                    speechData["qwenMaxBufferedAudioMiB"].get<int>();
            }
            if (speechData.contains("qwenMinimumFreeVramMiB"))
            {
                outSettings.speech.qwenMinimumFreeVramMiB =
                    speechData["qwenMinimumFreeVramMiB"].get<int>();
            }
            if (speechData.contains("qwenVoiceDesignModel"))
            {
                outSettings.speech.qwenVoiceDesignModel =
                    speechData["qwenVoiceDesignModel"].get<std::string>();
            }
            if (speechData.contains("qwenCloneModel"))
            {
                outSettings.speech.qwenCloneModel =
                    speechData["qwenCloneModel"].get<std::string>();
            }
            if (speechData.contains("voiceDataPath"))
            {
                outSettings.speech.voiceDataPath =
                    speechData["voiceDataPath"].get<std::string>();
            }
            if (speechData.contains("volume"))
            {
                outSettings.speech.volume = speechData["volume"].get<int>();
            }
            if (speechData.contains("rate"))
            {
                outSettings.speech.rate = speechData["rate"].get<int>();
            }
            if (speechData.contains("maxCharacters"))
            {
                outSettings.speech.maxCharacters = speechData["maxCharacters"].get<int>();
            }
            if (speechData.contains("maxQueuedUtterances"))
            {
                outSettings.speech.maxQueuedUtterances =
                    speechData["maxQueuedUtterances"].get<int>();
            }
        }

        if (data.contains("speechRecognition"))
        {
            const json& recognitionData = data["speechRecognition"];
            if (recognitionData.contains("enabled"))
            {
                outSettings.speechRecognition.bEnabled = recognitionData["enabled"].get<bool>();
            }
            if (recognitionData.contains("executable"))
            {
                outSettings.speechRecognition.executable =
                    recognitionData["executable"].get<std::string>();
            }
            if (recognitionData.contains("useServer"))
            {
                outSettings.speechRecognition.bUseServer =
                    recognitionData["useServer"].get<bool>();
            }
            if (recognitionData.contains("serverExecutable"))
            {
                outSettings.speechRecognition.serverExecutable =
                    recognitionData["serverExecutable"].get<std::string>();
            }
            if (recognitionData.contains("serverHost"))
            {
                outSettings.speechRecognition.serverHost =
                    recognitionData["serverHost"].get<std::string>();
            }
            if (recognitionData.contains("serverPort"))
            {
                outSettings.speechRecognition.serverPort =
                    recognitionData["serverPort"].get<int>();
            }
            if (recognitionData.contains("serverStartupTimeoutSeconds"))
            {
                outSettings.speechRecognition.serverStartupTimeoutSeconds =
                    recognitionData["serverStartupTimeoutSeconds"].get<int>();
            }
            if (recognitionData.contains("requestTimeoutSeconds"))
            {
                outSettings.speechRecognition.requestTimeoutSeconds =
                    recognitionData["requestTimeoutSeconds"].get<int>();
            }
            if (recognitionData.contains("modelPath"))
            {
                outSettings.speechRecognition.modelPath =
                    recognitionData["modelPath"].get<std::string>();
            }
            if (recognitionData.contains("language"))
            {
                outSettings.speechRecognition.language =
                    recognitionData["language"].get<std::string>();
            }
            if (recognitionData.contains("sampleRate"))
            {
                outSettings.speechRecognition.sampleRate = recognitionData["sampleRate"].get<int>();
            }
            if (recognitionData.contains("threads"))
            {
                outSettings.speechRecognition.threads = recognitionData["threads"].get<int>();
            }
            if (recognitionData.contains("useGpu"))
            {
                outSettings.speechRecognition.bUseGpu = recognitionData["useGpu"].get<bool>();
            }
            if (recognitionData.contains("device"))
            {
                outSettings.speechRecognition.device =
                    recognitionData["device"].get<std::string>();
            }
            if (recognitionData.contains("handsFree"))
            {
                outSettings.speechRecognition.bHandsFree =
                    recognitionData["handsFree"].get<bool>();
            }
            if (recognitionData.contains("vadEnergyThreshold"))
            {
                outSettings.speechRecognition.vadEnergyThreshold =
                    recognitionData["vadEnergyThreshold"].get<int>();
            }
            if (recognitionData.contains("vadSpeechFrames"))
            {
                outSettings.speechRecognition.vadSpeechFrames =
                    recognitionData["vadSpeechFrames"].get<int>();
            }
            if (recognitionData.contains("vadSilenceMs"))
            {
                outSettings.speechRecognition.vadSilenceMs =
                    recognitionData["vadSilenceMs"].get<int>();
            }
            if (recognitionData.contains("minimumUtteranceMs"))
            {
                outSettings.speechRecognition.minimumUtteranceMs =
                    recognitionData["minimumUtteranceMs"].get<int>();
            }
            if (recognitionData.contains("maximumUtteranceSeconds"))
            {
                outSettings.speechRecognition.maximumUtteranceSeconds =
                    recognitionData["maximumUtteranceSeconds"].get<int>();
            }
        }

        if (data.contains("presence"))
        {
            const json& presenceData = data["presence"];
            const auto text = [&presenceData](const char* key, std::string& target)
            {
                if (presenceData.contains(key)) target = presenceData[key].get<std::string>();
            };
            const auto number = [&presenceData](const char* key, int& target)
            {
                if (presenceData.contains(key)) target = presenceData[key].get<int>();
            };
            if (presenceData.contains("enabled"))
                outSettings.presence.bEnabled = presenceData["enabled"].get<bool>();
            if (presenceData.contains("avatarBridgeEnabled"))
                outSettings.presence.bAvatarBridgeEnabled =
                    presenceData["avatarBridgeEnabled"].get<bool>();
            if (presenceData.contains("externalAdaptersEnabled"))
                outSettings.presence.bExternalAdaptersEnabled =
                    presenceData["externalAdaptersEnabled"].get<bool>();
            text("statePath", outSettings.presence.statePath);
            text("eventPath", outSettings.presence.eventPath);
            text("inboxPath", outSettings.presence.inboxPath);
            text("outboxPath", outSettings.presence.outboxPath);
            number("adapterPollMs", outSettings.presence.adapterPollMs);
            number("maxAdapterEventsPerMinute",
                outSettings.presence.maxAdapterEventsPerMinute);
            number("maxAdapterTextCharacters",
                outSettings.presence.maxAdapterTextCharacters);
            if (presenceData.contains("allowedAdapters") &&
                presenceData["allowedAdapters"].is_array())
            {
                outSettings.presence.allowedAdapters.clear();
                for (const auto& adapter : presenceData["allowedAdapters"])
                {
                    if (adapter.is_string())
                        outSettings.presence.allowedAdapters.push_back(adapter.get<std::string>());
                }
            }
        }

        if (data.contains("resources"))
        {
            const json& resourceData = data["resources"];
            if (resourceData.contains("autoPlan"))
            {
                outSettings.resources.bAutoPlan = resourceData["autoPlan"].get<bool>();
            }
            if (resourceData.contains("reserveLogicalCores"))
            {
                outSettings.resources.reserveLogicalCores =
                    resourceData["reserveLogicalCores"].get<int>();
            }
            if (resourceData.contains("minimumFreeRamMiB"))
            {
                outSettings.resources.minimumFreeRamMiB =
                    resourceData["minimumFreeRamMiB"].get<int>();
            }
            if (resourceData.contains("llamaPromptCacheMiB"))
            {
                outSettings.resources.llamaPromptCacheMiB =
                    resourceData["llamaPromptCacheMiB"].get<int>();
            }
            if (resourceData.contains("sqliteCacheMiB"))
            {
                outSettings.resources.sqliteCacheMiB =
                    resourceData["sqliteCacheMiB"].get<int>();
            }
            if (resourceData.contains("gpuReserveMiB"))
            {
                outSettings.resources.gpuReserveMiB =
                    resourceData["gpuReserveMiB"].get<int>();
            }
            if (resourceData.contains("usageSampleSeconds"))
            {
                outSettings.resources.usageSampleSeconds =
                    resourceData["usageSampleSeconds"].get<int>();
            }
            if (resourceData.contains("allowChatModelSplit"))
            {
                outSettings.resources.bAllowChatModelSplit =
                    resourceData["allowChatModelSplit"].get<bool>();
            }
            if (resourceData.contains("assignments"))
            {
                const json& assignments = resourceData["assignments"];
                if (assignments.contains("chat"))
                {
                    outSettings.resources.chat = assignments["chat"].get<std::string>();
                }
                if (assignments.contains("voice"))
                {
                    outSettings.resources.voice = assignments["voice"].get<std::string>();
                }
                if (assignments.contains("speechRecognition"))
                {
                    outSettings.resources.speechRecognition =
                        assignments["speechRecognition"].get<std::string>();
                }
                if (assignments.contains("embeddings"))
                {
                    outSettings.resources.embeddings =
                        assignments["embeddings"].get<std::string>();
                }
            }
        }
        else
        {
            // Backward-compatible migration for configurations written before the
            // cross-pipeline planner existed. Explicit service choices remain effective.
            outSettings.resources.voice = outSettings.speech.qwenDevice == "auto"
                ? "auto-secondary"
                : (outSettings.speech.qwenDevice == "cuda"
                    ? "cuda:0"
                    : outSettings.speech.qwenDevice);
            outSettings.resources.embeddings = outSettings.embedding.device == "none"
                ? "cpu"
                : outSettings.embedding.device;
            outSettings.resources.speechRecognition = !outSettings.speechRecognition.bUseGpu
                ? "cpu"
                : (outSettings.speechRecognition.device == "auto"
                    ? "auto-secondary"
                    : (outSettings.speechRecognition.device == "cuda"
                        ? "cuda:0"
                        : outSettings.speechRecognition.device));
        }

        if (data.contains("vision"))
        {
            const json& visionData = data["vision"];
            if (visionData.contains("enabled"))
            {
                outSettings.vision.bEnabled = visionData["enabled"].get<bool>();
            }
            if (visionData.contains("requireConfirmation"))
            {
                outSettings.vision.bRequireConfirmation =
                    visionData["requireConfirmation"].get<bool>();
            }
            if (visionData.contains("maxResponseTokens"))
            {
                outSettings.vision.maxResponseTokens = visionData["maxResponseTokens"].get<int>();
            }
            if (visionData.contains("continuousAwareness"))
            {
                outSettings.vision.bContinuousAwareness =
                    visionData["continuousAwareness"].get<bool>();
            }
            if (visionData.contains("awarenessDebounceMs"))
            {
                outSettings.vision.awarenessDebounceMs =
                    visionData["awarenessDebounceMs"].get<int>();
            }
            if (visionData.contains("awarenessMinimumIntervalMs"))
            {
                outSettings.vision.awarenessMinimumIntervalMs =
                    visionData["awarenessMinimumIntervalMs"].get<int>();
            }
            if (visionData.contains("awarenessMaxResponseTokens"))
            {
                outSettings.vision.awarenessMaxResponseTokens =
                    visionData["awarenessMaxResponseTokens"].get<int>();
            }
            if (visionData.contains("resolutionConfidence"))
            {
                outSettings.vision.resolutionConfidence =
                    visionData["resolutionConfidence"].get<double>();
            }
            if (visionData.contains("minimumNameAgreement"))
            {
                outSettings.vision.minimumNameAgreement =
                    visionData["minimumNameAgreement"].get<double>();
            }
            if (visionData.contains("ambiguityMargin"))
            {
                outSettings.vision.ambiguityMargin =
                    visionData["ambiguityMargin"].get<double>();
            }
            if (visionData.contains("maxResolverElements"))
            {
                outSettings.vision.maxResolverElements =
                    visionData["maxResolverElements"].get<int>();
            }
        }

        if (data.contains("initiative"))
        {
            const json& initiativeData = data["initiative"];
            if (initiativeData.contains("enabled"))
            {
                outSettings.initiative.bEnabled = initiativeData["enabled"].get<bool>();
            }
            if (initiativeData.contains("curiosityEnabled"))
            {
                outSettings.initiative.bCuriosityEnabled =
                    initiativeData["curiosityEnabled"].get<bool>();
            }
            if (initiativeData.contains("spontaneousSpeechEnabled"))
            {
                outSettings.initiative.bSpontaneousSpeechEnabled =
                    initiativeData["spontaneousSpeechEnabled"].get<bool>();
            }
            if (initiativeData.contains("speakWhenUserAway"))
            {
                outSettings.initiative.bSpeakWhenUserAway =
                    initiativeData["speakWhenUserAway"].get<bool>();
            }
            if (initiativeData.contains("autonomousLearningEnabled"))
            {
                outSettings.initiative.bAutonomousLearningEnabled =
                    initiativeData["autonomousLearningEnabled"].get<bool>();
            }
            if (initiativeData.contains("curiosityCheckSeconds"))
            {
                outSettings.initiative.curiosityCheckSeconds =
                    initiativeData["curiosityCheckSeconds"].get<int>();
            }
            if (initiativeData.contains("autonomousQuietSeconds"))
            {
                outSettings.initiative.autonomousQuietSeconds =
                    initiativeData["autonomousQuietSeconds"].get<int>();
            }
            if (initiativeData.contains("curiosityTopicCooldownMinutes"))
            {
                outSettings.initiative.curiosityTopicCooldownMinutes =
                    initiativeData["curiosityTopicCooldownMinutes"].get<int>();
            }
            if (initiativeData.contains("minimumConfidence"))
            {
                outSettings.initiative.minimumConfidence =
                    initiativeData["minimumConfidence"].get<float>();
            }
            if (initiativeData.contains("maxUtterancesPerHour"))
            {
                outSettings.initiative.maxUtterancesPerHour =
                    initiativeData["maxUtterancesPerHour"].get<int>();
            }
            if (initiativeData.contains("cooldownSeconds"))
            {
                outSettings.initiative.cooldownSeconds =
                    initiativeData["cooldownSeconds"].get<int>();
            }
            if (initiativeData.contains("dismissalCooldownSeconds"))
            {
                outSettings.initiative.dismissalCooldownSeconds =
                    initiativeData["dismissalCooldownSeconds"].get<int>();
            }
            if (initiativeData.contains("quietInputSeconds"))
            {
                outSettings.initiative.quietInputSeconds =
                    initiativeData["quietInputSeconds"].get<int>();
            }
            if (initiativeData.contains("minimumPrecision"))
            {
                outSettings.initiative.minimumPrecision =
                    initiativeData["minimumPrecision"].get<float>();
            }
            if (initiativeData.contains("suppressWhenFullScreen"))
            {
                outSettings.initiative.bSuppressWhenFullScreen =
                    initiativeData["suppressWhenFullScreen"].get<bool>();
            }
            if (initiativeData.contains("focusSessionMinutes"))
            {
                outSettings.initiative.focusSessionMinutes =
                    initiativeData["focusSessionMinutes"].get<int>();
            }
            if (initiativeData.contains("returnAfterMinutes"))
            {
                outSettings.initiative.returnAfterMinutes =
                    initiativeData["returnAfterMinutes"].get<int>();
            }
            if (initiativeData.contains("contextSwitchWindowSeconds"))
            {
                outSettings.initiative.contextSwitchWindowSeconds =
                    initiativeData["contextSwitchWindowSeconds"].get<int>();
            }
            if (initiativeData.contains("contextSwitchCount"))
            {
                outSettings.initiative.contextSwitchCount =
                    initiativeData["contextSwitchCount"].get<int>();
            }
            if (initiativeData.contains("cueMaxAgeMinutes"))
            {
                outSettings.initiative.cueMaxAgeMinutes =
                    initiativeData["cueMaxAgeMinutes"].get<int>();
            }
        }

        if (data.contains("bargeIn"))
        {
            const json& bargeInData = data["bargeIn"];
            if (bargeInData.contains("enabled"))
            {
                outSettings.bargeIn.bEnabled = bargeInData["enabled"].get<bool>();
            }
            if (bargeInData.contains("energyThreshold"))
            {
                outSettings.bargeIn.energyThreshold =
                    bargeInData["energyThreshold"].get<int>();
            }
            if (bargeInData.contains("echoMarginMultiplier"))
            {
                outSettings.bargeIn.echoMarginMultiplier =
                    bargeInData["echoMarginMultiplier"].get<float>();
            }
            if (bargeInData.contains("consecutiveFramesRequired"))
            {
                outSettings.bargeIn.consecutiveFramesRequired =
                    bargeInData["consecutiveFramesRequired"].get<int>();
            }
            if (bargeInData.contains("startupGraceMs"))
            {
                outSettings.bargeIn.startupGraceMs =
                    bargeInData["startupGraceMs"].get<int>();
            }
        }

        if (data.contains("perception"))
        {
            const json& perceptionData = data["perception"];
            if (perceptionData.contains("enabled"))
            {
                outSettings.perception.bEnabled = perceptionData["enabled"].get<bool>();
            }
            if (perceptionData.contains("minimumEventIntervalMs"))
            {
                outSettings.perception.minimumEventIntervalMs =
                    perceptionData["minimumEventIntervalMs"].get<int>();
            }
            if (perceptionData.contains("maxObservationsPerMinute"))
            {
                outSettings.perception.maxObservationsPerMinute =
                    perceptionData["maxObservationsPerMinute"].get<int>();
            }
            // Configured lists extend the defaults rather than replacing them. A config
            // that names one more password manager must not silently drop the rest of the
            // deny list, which is what assignment would do.
            if (perceptionData.contains("excludedApplications") &&
                perceptionData["excludedApplications"].is_array())
            {
                for (const auto& entry : perceptionData["excludedApplications"])
                {
                    if (entry.is_string())
                    {
                        outSettings.perception.excludedApplications.push_back(
                            entry.get<std::string>());
                    }
                }
            }
            if (perceptionData.contains("excludedTitleFragments") &&
                perceptionData["excludedTitleFragments"].is_array())
            {
                for (const auto& entry : perceptionData["excludedTitleFragments"])
                {
                    if (entry.is_string())
                    {
                        outSettings.perception.excludedTitleFragments.push_back(
                            entry.get<std::string>());
                    }
                }
            }
        }

        if (data.contains("image"))
        {
            const json& imageData = data["image"];
            const auto text = [&imageData](const char* key, std::string& target)
            {
                if (imageData.contains(key)) target = imageData[key].get<std::string>();
            };
            const auto number = [&imageData](const char* key, int& target)
            {
                if (imageData.contains(key)) target = imageData[key].get<int>();
            };
            if (imageData.contains("enabled"))
            {
                outSettings.image.bEnabled = imageData["enabled"].get<bool>();
            }
            text("pythonExecutable", outSettings.image.pythonExecutable);
            text("serviceScript", outSettings.image.serviceScript);
            text("cacheDirectory", outSettings.image.cacheDirectory);
            text("outputPath", outSettings.image.outputPath);
            text("host", outSettings.image.host);
            text("model", outSettings.image.model);
            text("device", outSettings.image.device);
            number("port", outSettings.image.port);
            number("minimumFreeVramMiB", outSettings.image.minimumFreeVramMiB);
            number("steps", outSettings.image.steps);
            number("width", outSettings.image.width);
            number("height", outSettings.image.height);
            number("startupTimeoutSeconds", outSettings.image.startupTimeoutSeconds);
            number("requestTimeoutSeconds", outSettings.image.requestTimeoutSeconds);
            if (imageData.contains("guidance"))
            {
                outSettings.image.guidance = imageData["guidance"].get<float>();
            }
            if (imageData.contains("shutdownOnExit"))
            {
                outSettings.image.bShutdownOnExit = imageData["shutdownOnExit"].get<bool>();
            }
        }
        if (data.contains("conversation"))
        {
            const json& conversationData = data["conversation"];
            if (conversationData.contains("archiveEnabled"))
            {
                outSettings.conversation.bArchiveEnabled =
                    conversationData["archiveEnabled"].get<bool>();
            }
            if (conversationData.contains("maxSessions"))
            {
                outSettings.conversation.maxSessions =
                    conversationData["maxSessions"].get<int>();
            }
            if (conversationData.contains("maxTurnsPerSession"))
            {
                outSettings.conversation.maxTurnsPerSession =
                    conversationData["maxTurnsPerSession"].get<int>();
            }
            if (conversationData.contains("maxTurnCharacters"))
            {
                outSettings.conversation.maxTurnCharacters =
                    conversationData["maxTurnCharacters"].get<int>();
            }
            if (conversationData.contains("restoreTurns"))
            {
                outSettings.conversation.restoreTurns =
                    conversationData["restoreTurns"].get<int>();
            }
        }
        if (data.contains("responseFilter"))
        {
            const json& filterData = data["responseFilter"];
            if (filterData.contains("aiReviewEnabled"))
            {
                outSettings.responseFilter.bAiReviewEnabled =
                    filterData["aiReviewEnabled"].get<bool>();
            }
            if (filterData.contains("aiMaxReviewTokens"))
            {
                outSettings.responseFilter.aiMaxReviewTokens =
                    filterData["aiMaxReviewTokens"].get<int>();
            }
            if (filterData.contains("maxReplyCharacters"))
            {
                outSettings.responseFilter.maxReplyCharacters =
                    filterData["maxReplyCharacters"].get<int>();
            }
        }
        if (data.contains("inputArbiter"))
        {
            const json& arbiterData = data["inputArbiter"];
            if (arbiterData.contains("mergeWindowMs"))
            {
                outSettings.inputArbiter.mergeWindowMs =
                    arbiterData["mergeWindowMs"].get<int>();
            }
            if (arbiterData.contains("minimumMeaningfulCharacters"))
            {
                outSettings.inputArbiter.minimumMeaningfulCharacters =
                    arbiterData["minimumMeaningfulCharacters"].get<int>();
            }
            if (arbiterData.contains("maxQueuedInputs"))
            {
                outSettings.inputArbiter.maxQueuedInputs =
                    arbiterData["maxQueuedInputs"].get<int>();
            }
            if (arbiterData.contains("ignoredFragments") &&
                arbiterData["ignoredFragments"].is_array())
            {
                for (const auto& entry : arbiterData["ignoredFragments"])
                {
                    if (entry.is_string())
                    {
                        outSettings.inputArbiter.ignoredFragments.push_back(
                            entry.get<std::string>());
                    }
                }
            }
        }
    }
    catch (const std::exception&)
    {
        return false;
    }

    const bool intelligencePortsConflict = outSettings.intelligence.bEnabled &&
        ((outSettings.intelligence.fast.bEnabled &&
            (outSettings.intelligence.fast.port == outSettings.llm.port ||
             outSettings.intelligence.fast.port == outSettings.embedding.port ||
             outSettings.intelligence.fast.port == outSettings.speech.qwenPort ||
             outSettings.intelligence.fast.port == outSettings.speechRecognition.serverPort)) ||
         (outSettings.intelligence.expert.bEnabled &&
            (outSettings.intelligence.expert.port == outSettings.llm.port ||
             outSettings.intelligence.expert.port == outSettings.embedding.port ||
             outSettings.intelligence.expert.port == outSettings.speech.qwenPort ||
             outSettings.intelligence.expert.port ==
                outSettings.speechRecognition.serverPort)) ||
         (outSettings.intelligence.fast.bEnabled &&
          outSettings.intelligence.expert.bEnabled &&
          outSettings.intelligence.fast.port == outSettings.intelligence.expert.port));

    if (outSettings.activeProfile.empty() || outSettings.llm.host.empty() ||
        outSettings.llm.modelName.empty() || outSettings.llm.port < 1 ||
        outSettings.llm.port > 65535 || outSettings.llm.temperature < 0.0f ||
        outSettings.llm.temperature > 2.0f || outSettings.llm.maxTokens < 1 ||
        outSettings.llm.maxTokens > 32768 ||
        outSettings.llm.contextSize < 512 || outSettings.llm.contextSize > 1048576 ||
        outSettings.llm.parallelRequests < 1 || outSettings.llm.parallelRequests > 16 ||
        outSettings.llm.autoFitTargetMiB < 256 ||
        outSettings.llm.autoFitTargetMiB > 32768 ||
        outSettings.llm.startupTimeoutSeconds < 1 ||
        outSettings.llm.startupTimeoutSeconds > 600 ||
        (outSettings.llm.backend == "LLamaCpp" && outSettings.llm.bAutoStartServer &&
            (outSettings.llm.serverExecutable.empty() || outSettings.llm.modelPath.empty())) ||
        (outSettings.llm.bVisionEnabled &&
            (outSettings.llm.multimodalProjectorPath.empty() || outSettings.llm.mediaPath.empty())) ||
        (outSettings.intelligence.bEnabled &&
            (!IsValidModelTier(outSettings.intelligence.fast) ||
             !IsValidModelTier(outSettings.intelligence.expert) ||
             intelligencePortsConflict)) ||
        (outSettings.embedding.bEnabled &&
            (outSettings.embedding.host.empty() || outSettings.embedding.modelName.empty() ||
                outSettings.embedding.port < 1 || outSettings.embedding.port > 65535 ||
                outSettings.embedding.port == outSettings.llm.port ||
                outSettings.embedding.contextSize < 128 ||
                outSettings.embedding.contextSize > 1048576 ||
                outSettings.embedding.parallelRequests < 1 ||
                outSettings.embedding.parallelRequests > 16 ||
                outSettings.embedding.startupTimeoutSeconds < 1 ||
                outSettings.embedding.startupTimeoutSeconds > 600 ||
                outSettings.embedding.pooling != "mean" ||
                outSettings.embedding.device.empty() ||
                outSettings.embedding.queryPrefix.empty() ||
                outSettings.embedding.documentPrefix.empty() ||
                (outSettings.embedding.bAutoStartServer &&
                    (outSettings.embedding.serverExecutable.empty() ||
                        outSettings.embedding.modelPath.empty())))) ||
        outSettings.speech.volume < 0 || outSettings.speech.volume > 100 ||
        (outSettings.speech.backend != "Auto" &&
            outSettings.speech.backend != "WindowsSapi" &&
            outSettings.speech.backend != "Qwen3TTS") ||
        outSettings.speech.pythonExecutable.empty() ||
        outSettings.speech.qwenServiceScript.empty() ||
        outSettings.speech.qwenHost != "127.0.0.1" ||
        outSettings.speech.qwenPort < 1 || outSettings.speech.qwenPort > 65535 ||
        outSettings.speech.qwenPort == outSettings.llm.port ||
        outSettings.speech.qwenPort == outSettings.embedding.port ||
        outSettings.speech.qwenStartupTimeoutSeconds < 1 ||
        outSettings.speech.qwenStartupTimeoutSeconds > 300 ||
        outSettings.speech.qwenRequestTimeoutSeconds < 30 ||
        outSettings.speech.qwenRequestTimeoutSeconds > 3600 ||
        outSettings.speech.qwenMinimumFreeVramMiB < 512 ||
        outSettings.speech.qwenMinimumFreeVramMiB > 65536 ||
        outSettings.speech.qwenDevices.empty() ||
        std::any_of(outSettings.speech.qwenDevices.begin(),
            outSettings.speech.qwenDevices.end(),
            [](const std::string& device) { return !IsDeviceSelector(device, true); }) ||
        outSettings.speech.qwenMaxWorkers < 1 || outSettings.speech.qwenMaxWorkers > 8 ||
        outSettings.speech.qwenPrefetchFragments < 1 ||
        outSettings.speech.qwenPrefetchFragments > 16 ||
        outSettings.speech.qwenPhraseCharacters < 48 ||
        outSettings.speech.qwenPhraseCharacters > 512 ||
        outSettings.speech.qwenMaxBufferedAudioMiB < 16 ||
        outSettings.speech.qwenMaxBufferedAudioMiB > 2048 ||
        outSettings.speech.qwenVoiceDesignModel.empty() ||
        outSettings.speech.qwenCloneModel.empty() ||
        outSettings.speech.voiceDataPath.empty() ||
        outSettings.speech.rate < -10 || outSettings.speech.rate > 10 ||
        outSettings.speech.maxCharacters < 64 || outSettings.speech.maxCharacters > 10000 ||
        outSettings.speech.maxQueuedUtterances < 1 ||
        outSettings.speech.maxQueuedUtterances > 16 ||
        (outSettings.speechRecognition.bEnabled &&
            (outSettings.speechRecognition.executable.empty() ||
                outSettings.speechRecognition.modelPath.empty() ||
                outSettings.speechRecognition.language.empty() ||
                (outSettings.speechRecognition.bUseServer &&
                    (outSettings.speechRecognition.serverExecutable.empty() ||
                     outSettings.speechRecognition.serverHost != "127.0.0.1" ||
                     outSettings.speechRecognition.serverPort < 1 ||
                     outSettings.speechRecognition.serverPort > 65535 ||
                     outSettings.speechRecognition.serverPort == outSettings.llm.port ||
                     outSettings.speechRecognition.serverPort == outSettings.embedding.port ||
                     outSettings.speechRecognition.serverPort == outSettings.speech.qwenPort ||
                     outSettings.speechRecognition.serverStartupTimeoutSeconds < 1 ||
                     outSettings.speechRecognition.serverStartupTimeoutSeconds > 300 ||
                     outSettings.speechRecognition.requestTimeoutSeconds < 10 ||
                     outSettings.speechRecognition.requestTimeoutSeconds > 1800)) ||
                outSettings.speechRecognition.sampleRate != 16000 ||
                outSettings.speechRecognition.threads < 1 ||
                outSettings.speechRecognition.threads > 64 ||
                outSettings.speechRecognition.device.empty() ||
                outSettings.speechRecognition.vadEnergyThreshold < 100 ||
                outSettings.speechRecognition.vadEnergyThreshold > 30000 ||
                outSettings.speechRecognition.vadSpeechFrames < 1 ||
                outSettings.speechRecognition.vadSpeechFrames > 50 ||
                outSettings.speechRecognition.vadSilenceMs < 200 ||
                outSettings.speechRecognition.vadSilenceMs > 5000 ||
                outSettings.speechRecognition.minimumUtteranceMs < 100 ||
                outSettings.speechRecognition.minimumUtteranceMs > 5000 ||
                outSettings.speechRecognition.maximumUtteranceSeconds < 2 ||
                outSettings.speechRecognition.maximumUtteranceSeconds > 120)) ||
        outSettings.presence.statePath.empty() ||
        outSettings.presence.eventPath.empty() ||
        outSettings.presence.inboxPath.empty() ||
        outSettings.presence.outboxPath.empty() ||
        outSettings.presence.adapterPollMs < 50 ||
        outSettings.presence.adapterPollMs > 10000 ||
        outSettings.presence.maxAdapterEventsPerMinute < 1 ||
        outSettings.presence.maxAdapterEventsPerMinute > 600 ||
        outSettings.presence.maxAdapterTextCharacters < 64 ||
        outSettings.presence.maxAdapterTextCharacters > 20000 ||
        outSettings.presence.allowedAdapters.empty() ||
        outSettings.resources.reserveLogicalCores < 0 ||
        outSettings.resources.reserveLogicalCores > 64 ||
        outSettings.resources.minimumFreeRamMiB < 1024 ||
        outSettings.resources.minimumFreeRamMiB > 262144 ||
        outSettings.resources.llamaPromptCacheMiB < 0 ||
        outSettings.resources.llamaPromptCacheMiB > 65536 ||
        outSettings.resources.sqliteCacheMiB < 0 ||
        outSettings.resources.sqliteCacheMiB > 2048 ||
        outSettings.resources.gpuReserveMiB < 256 ||
        outSettings.resources.gpuReserveMiB > 32768 ||
        outSettings.resources.usageSampleSeconds < 0 ||
        outSettings.resources.usageSampleSeconds > 3600 ||
        outSettings.conversation.maxSessions < 1 ||
        outSettings.conversation.maxSessions > 10000 ||
        outSettings.conversation.maxTurnsPerSession < 1 ||
        outSettings.conversation.maxTurnsPerSession > 100000 ||
        outSettings.conversation.maxTurnCharacters < 256 ||
        outSettings.conversation.maxTurnCharacters > 1000000 ||
        outSettings.conversation.restoreTurns < 0 ||
        outSettings.conversation.restoreTurns > 40 ||
        outSettings.responseFilter.aiMaxReviewTokens < 64 ||
        outSettings.responseFilter.aiMaxReviewTokens > 512 ||
        outSettings.responseFilter.maxReplyCharacters < 256 ||
        outSettings.responseFilter.maxReplyCharacters > 100000 ||
        outSettings.image.port < 1 || outSettings.image.port > 65535 ||
        outSettings.image.steps < 1 || outSettings.image.steps > 100 ||
        outSettings.image.width < 256 || outSettings.image.width > 1024 ||
        outSettings.image.height < 256 || outSettings.image.height > 1024 ||
        outSettings.image.host.empty() || outSettings.image.model.empty() ||
        !IsDeviceSelector(outSettings.image.device, true) ||
        outSettings.resources.chat.empty() ||
        outSettings.resources.voice.empty() ||
        outSettings.resources.speechRecognition.empty() ||
        outSettings.resources.embeddings.empty() ||
        !IsDeviceSelector(outSettings.resources.chat, false) ||
        !IsDeviceSelector(outSettings.resources.voice, true) ||
        !IsDeviceSelector(outSettings.resources.speechRecognition, true) ||
        !IsDeviceSelector(outSettings.resources.embeddings, false) ||
        !IsDeviceSelector(outSettings.speechRecognition.device, true) ||
        outSettings.vision.maxResponseTokens < 64 ||
        outSettings.vision.maxResponseTokens > 4096 ||
        outSettings.vision.awarenessDebounceMs < 250 ||
        outSettings.vision.awarenessDebounceMs > 60000 ||
        outSettings.vision.awarenessMinimumIntervalMs < 1000 ||
        outSettings.vision.awarenessMinimumIntervalMs > 3600000 ||
        outSettings.vision.awarenessMaxResponseTokens < 64 ||
        outSettings.vision.awarenessMaxResponseTokens > 512 ||
        outSettings.vision.resolutionConfidence < 0.5 ||
        outSettings.vision.resolutionConfidence > 1.0 ||
        outSettings.vision.minimumNameAgreement < 0.1 ||
        outSettings.vision.minimumNameAgreement > 1.0 ||
        outSettings.vision.ambiguityMargin < 0.0 ||
        outSettings.vision.ambiguityMargin > 0.5 ||
        outSettings.vision.maxResolverElements < 25 ||
        outSettings.vision.maxResolverElements > 5000 ||
        outSettings.inputArbiter.mergeWindowMs < 0 ||
        outSettings.inputArbiter.mergeWindowMs > 10000 ||
        outSettings.inputArbiter.minimumMeaningfulCharacters < 1 ||
        outSettings.inputArbiter.minimumMeaningfulCharacters > 64 ||
        outSettings.inputArbiter.maxQueuedInputs < 1 ||
        outSettings.inputArbiter.maxQueuedInputs > 64 ||
        // Fails closed like every other section: a config that would observe faster than
        // a human can switch windows is rejected rather than quietly clamped into
        // something reasonable.
        outSettings.perception.minimumEventIntervalMs < 100 ||
        outSettings.perception.minimumEventIntervalMs > 60000 ||
        outSettings.perception.maxObservationsPerMinute < 1 ||
        outSettings.perception.maxObservationsPerMinute > 600 ||
        // Config can only extend the deny lists, never shorten them, so this cannot fire
        // from a settings file. It guards the built-in defaults themselves: if those are
        // ever emptied in code, perception must refuse to start rather than watch
        // everything.
        (outSettings.perception.bEnabled &&
            (outSettings.perception.excludedApplications.empty() ||
                outSettings.perception.excludedTitleFragments.empty())) ||
        // Speaking first fails closed too. A confidence floor at zero or an unbounded
        // hourly rate would turn "may occasionally offer something" into a nuisance, and
        // a config typo should not be the thing that decides that.
        outSettings.initiative.minimumConfidence < 0.1f ||
        outSettings.initiative.minimumConfidence > 1.0f ||
        outSettings.initiative.maxUtterancesPerHour < 1 ||
        outSettings.initiative.maxUtterancesPerHour > 30 ||
        outSettings.initiative.cooldownSeconds < 30 ||
        outSettings.initiative.quietInputSeconds < 1 ||
        outSettings.initiative.curiosityCheckSeconds < 5 ||
        outSettings.initiative.curiosityCheckSeconds > 3600 ||
        outSettings.initiative.autonomousQuietSeconds < 5 ||
        outSettings.initiative.autonomousQuietSeconds > 3600 ||
        outSettings.initiative.curiosityTopicCooldownMinutes < 10 ||
        outSettings.initiative.curiosityTopicCooldownMinutes > 10080 ||
        outSettings.initiative.focusSessionMinutes < 2 ||
        outSettings.initiative.focusSessionMinutes > 240 ||
        outSettings.initiative.returnAfterMinutes < 2 ||
        outSettings.initiative.returnAfterMinutes > 1440 ||
        outSettings.initiative.contextSwitchWindowSeconds < 60 ||
        outSettings.initiative.contextSwitchWindowSeconds > 3600 ||
        outSettings.initiative.contextSwitchCount < 4 ||
        outSettings.initiative.contextSwitchCount > 30 ||
        outSettings.initiative.cueMaxAgeMinutes < 1 ||
        outSettings.initiative.cueMaxAgeMinutes > 60 ||
        outSettings.initiative.minimumPrecision < 0.0f ||
        outSettings.initiative.minimumPrecision > 1.0f ||
        outSettings.bargeIn.energyThreshold < 100 ||
        outSettings.bargeIn.energyThreshold > 30000 ||
        outSettings.bargeIn.consecutiveFramesRequired < 1 ||
        outSettings.bargeIn.consecutiveFramesRequired > 200 ||
        // Below 1.0 the adaptive threshold would sit under the measured echo, which is
        // the configuration that makes Revia interrupt herself.
        outSettings.bargeIn.echoMarginMultiplier < 1.2f ||
        outSettings.bargeIn.echoMarginMultiplier > 20.0f ||
        outSettings.bargeIn.startupGraceMs < 200 ||
        outSettings.bargeIn.startupGraceMs > 10000)
    {
        return false;
    }

    return true;
}

bool configManager::LoadProfile(const std::string& profileId, aiProfile& outProfile) const
{
    // M2: profileId comes from user input (/profile <name>). Reject anything
    // that could escape the Config/Profiles directory.
    if (profileId.empty() ||
        profileId.find('/') != std::string::npos ||
        profileId.find('\\') != std::string::npos ||
        profileId.find("..") != std::string::npos)
    {
        return false;
    }

    const std::string profileFile = profilePath + "/" + profileId + ".json";

    std::ifstream file(profileFile);

    if (!file.is_open())
    {
        return false;
    }

    // H1: Guard against malformed JSON / wrongly-typed values.
    try
    {
        json data;
        file >> data;

        if (data.contains("id"))
        {
            outProfile.id = data["id"].get<std::string>();
        }

        if (data.contains("displayName"))
        {
            outProfile.displayName = data["displayName"].get<std::string>();
        }

        if (data.contains("systemPrompt"))
        {
            outProfile.systemPrompt = data["systemPrompt"].get<std::string>();
        }

        // H3: memoryEnabled was previously ignored, so profiles that disable
        // memory had no effect. Parse it into the profile.
        if (data.contains("memoryEnabled"))
        {
            outProfile.bMemoryEnabled = data["memoryEnabled"].get<bool>();
        }

        if (data.contains("temperature"))
        {
            outProfile.temperature = data["temperature"].get<float>();
            outProfile.bHasTemperatureOverride = true;
        }

        if (data.contains("maxTokens"))
        {
            outProfile.maxTokens = data["maxTokens"].get<int>();
            outProfile.bHasMaxTokensOverride = true;
        }
    }
    catch (const std::exception&)
    {
        return false;
    }

    if (outProfile.id.empty() || outProfile.displayName.empty() ||
        outProfile.systemPrompt.empty() ||
        (outProfile.bHasTemperatureOverride &&
            (outProfile.temperature < 0.0f || outProfile.temperature > 2.0f)) ||
        (outProfile.bHasMaxTokensOverride &&
            (outProfile.maxTokens < 1 || outProfile.maxTokens > 32768)))
    {
        return false;
    }

    return true;
}
