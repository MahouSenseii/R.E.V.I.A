#include "Core/configManager.h"

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

        if (llmData.contains("temperature"))
        {
            outSettings.llm.temperature = llmData["temperature"].get<float>();
        }

        if (llmData.contains("maxTokens"))
        {
            outSettings.llm.maxTokens = llmData["maxTokens"].get<int>();
        }
    }

    return true;
}

bool configManager::LoadProfile(const std::string& profileId, aiProfile& outProfile) const
{
    const std::string profileFile = profilePath + "/" + profileId + ".json";

    std::ifstream file(profileFile);

    if (!file.is_open())
    {
        return false;
    }

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

    return true;
}
