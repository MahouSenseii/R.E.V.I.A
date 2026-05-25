#include "LLM/llmService.h"

llmService::llmService() = default;

llmService::~llmService() = default;

void llmService::ApplySettings(const llmSettings &settings, const aiProfile &profile)
{
    if (settings.backend == "LLamaCpp")
    {
        backendType = llmBackendType::LLamaCpp;
        llamaCpp.ApplySettings(settings, profile);
        bIsReady = true;
        return;
    }

    if (settings.backend == "Placeholder")
    {
        backendType = llmBackendType::Placeholder;
        bIsReady = true;
        return;
    }

    backendType = llmBackendType::None;
    bIsReady = false;
}

bool llmService::IsBackendAvailable() const
{
    switch (backendType)
    {
        case llmBackendType::LLamaCpp:
            return llamaCpp.IsServerAvailable();

        case llmBackendType::Placeholder:
            return true;

        case llmBackendType::None:
        default:
            return false;
    }
}

healthOutput llmService::CheckBackendHealth() const
{
    switch (backendType)
    {
        case llmBackendType::LLamaCpp:
            return llamaCpp.CheckHealth();

        case llmBackendType::Placeholder:
        {
            healthOutput output;
            output.bIsAvailable = true;
            output.status = systemStatus::Green;
            output.name = "Placeholder";
            output.message = "Placeholder backend is available.";
            return output;
        }

        case llmBackendType::None:
        default:
        {
            healthOutput output;
            output.bIsAvailable = false;
            output.status = systemStatus::Red;
            output.name = "None";
            output.message = "No LLM backend is enabled.";
            output.reason = "Backend type is None or unsupported.";
            return output;
        }
    }
}

responseOutput llmService::GenerateResponse(const std::string& prompt,const std::vector<conversationMessage>& context) const
{
    if (!bIsReady)
    {
        responseOutput output;
        output.bSuccess = false;
        output.response = "My language system is not ready yet.";
        output.reason = "LLM service was not ready.";
        output.bShouldSpeak = true;
        output.bShouldRemember = false;

        return output;
    }

    switch (backendType)
    {
        case llmBackendType::Placeholder:
            return GeneratePlaceholderResponse(prompt);

        case llmBackendType::LLamaCpp:
            return llamaCpp.GenerateResponse(prompt, context);

        case llmBackendType::None:
        default:
        {
            responseOutput output;
            output.bSuccess = false;
            output.response = "No language backend is enabled.";
            output.reason = "Unsupported or disabled LLM backend.";
            output.bShouldSpeak = true;
            output.bShouldRemember = false;

            return output;
        }
    }
}

responseOutput llmService::GeneratePlaceholderResponse(const std::string& prompt) const
{
    responseOutput output;

    output.bSuccess = true;
    output.response = "LLM placeholder response to: " + prompt;
    output.reason = "";
    output.bShouldSpeak = true;
    output.bShouldRemember = false;

    return output;
}