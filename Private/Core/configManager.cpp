#include "Core/configManager.h"

#include <exception>
#include <fstream>
#include <nlohmann/json.hpp>

#include "Library/structLibrary.h"

using json = nlohmann::json;

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
        }

        if (data.contains("initiative"))
        {
            const json& initiativeData = data["initiative"];
            if (initiativeData.contains("enabled"))
            {
                outSettings.initiative.bEnabled = initiativeData["enabled"].get<bool>();
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
    }
    catch (const std::exception&)
    {
        return false;
    }

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
                outSettings.speechRecognition.sampleRate != 16000 ||
                outSettings.speechRecognition.threads < 1 ||
                outSettings.speechRecognition.threads > 64)) ||
        outSettings.vision.maxResponseTokens < 64 ||
        outSettings.vision.maxResponseTokens > 4096 ||
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
