#include "Core/commandManager.h"
#include <sstream>

#include "Core/configManager.h"
#include "Core/messageRouter.h"

commandManager::commandManager()= default;

commandManager::~commandManager() = default;

commandOutput commandManager::HandleCommand(const std::string& input,appSettings& settings,
    aiProfile& profile,configManager& config,messageRouter& router) const
{
    commandOutput output;

    if (!IsCommand(input))
    {
        output.bWasCommand = false;
        return output;
    }

    output.bWasCommand = true;

    if (input == "/help")
    {
        return BuildHelpOutput();
    }

    if (input == "/status")
    {
        return BuildStatusOutput(settings, profile, router.CheckLLMHealth());
    }

    if (input == "/backend")
    {
        const healthOutput health = router.CheckLLMHealth();

        commandOutput output;
        output.bWasCommand = true;

        std::ostringstream stream;
        stream << "\n========== Backend Health ==========\n";
        stream << "Name:      " << health.name << "\n";
        stream << "Status:    " << StatusToString(health.status) << "\n";
        stream << "Available: " << (health.bIsAvailable ? "true" : "false") << "\n";
        stream << "Message:   " << health.message << "\n";

        if (!health.reason.empty())
        {
            stream << "Reason:    " << health.reason << "\n";
        }

        stream << "====================================\n";

        output.output = stream.str();
        return output;
    }

    if (input == "/exit" || input == "/quit" || input == "/bye")
    {
        output.bShouldExit = true;
        output.output = "Exiting R.E.V.I.A...";
        return output;
    }

    if (input == "/profile")
    {
        output.output = "Usage: /profile <profileName>\nExample: /profile revia";
        return output;
    }

    if (input.rfind("/profile ", 0) == 0)
    {
        return HandleProfileCommand(input, settings, profile, config, router);
    }

    output.bSuccess = false;
    output.output = "Unknown command. Type /help for available commands.";
    output.reason = "Unknown command: " + input;

    return output;
}

bool commandManager::IsCommand(const std::string &input) const
{
    return !input.empty() && input[0] == '/';
}

std::string commandManager::StatusToString(systemStatus status)
{
    switch (status)
    {
        case systemStatus::Green:
            return "Green";
        case systemStatus::Yellow:
            return "Yellow";
        case systemStatus::Red:
            return "Red";
        default:
            return "Unknown";
    }
}

commandOutput commandManager::BuildHelpOutput() const
{
    commandOutput output;
    output.bWasCommand = true;

    std::ostringstream stream;

    stream << "\n========== R.E.V.I.A Commands ==========\n";
    stream << "/help     - Show available commands\n";
    stream << "/status   - Show loaded profile and LLM settings\n";
    stream << "/backend  - Show LLM backend health\n";
    stream << "/profile  - Change active profile\n";
    stream << "/exit     - Exit the application\n";
    stream << "/quit     - Exit the application\n";
    stream << "========================================\n";

    output.output = stream.str();

    return output;
}

commandOutput commandManager::BuildStatusOutput(const appSettings &settings, const aiProfile &profile, const healthOutput& llmHealth) const
{
    commandOutput output;
    output.bWasCommand = true;

    std::ostringstream stream;

    stream << "\n========== R.E.V.I.A Status ==========\n";

    stream << "Active Profile: " << settings.activeProfile << "\n";
    stream << "Profile Name:   " << profile.displayName << "\n";
    stream << "Profile ID:     " << profile.id << "\n";

    stream << "\nLLM Settings\n";
    stream << "Backend:        " << settings.llm.backend << "\n";
    stream << "Host:           " << settings.llm.host << "\n";
    stream << "Port:           " << settings.llm.port << "\n";
    stream << "Model Name:     " << settings.llm.modelName << "\n";
    stream << "Temperature:    " << settings.llm.temperature << "\n";
    stream << "Max Tokens:     " << settings.llm.maxTokens << "\n";

    stream << "\nBackend Health\n";
    stream << "Name:           " << llmHealth.name << "\n";
    stream << "Available:      " << (llmHealth.bIsAvailable ? "true" : "false") << "\n";
    stream << "Message:        " << llmHealth.message << "\n";
    stream << "Status:         " << StatusToString(llmHealth.status) << "\n";
    if (!llmHealth.reason.empty())
    {
        stream << "Reason:         " << llmHealth.reason << "\n";
    }

    stream << "\nProfile Overrides\n";
    stream << "Temperature Override: "
           << (profile.bHasTemperatureOverride ? "true" : "false") << "\n";

    stream << "Max Tokens Override:  "
           << (profile.bHasMaxTokensOverride ? "true" : "false") << "\n";

    if (profile.bHasTemperatureOverride)
    {
        stream << "Profile Temperature:  " << profile.temperature << "\n";
    }

    if (profile.bHasMaxTokensOverride)
    {
        stream << "Profile Max Tokens:   " << profile.maxTokens << "\n";
    }

    stream << "======================================\n";

    output.output = stream.str();

    return output;
}



commandOutput commandManager::HandleProfileCommand(const std::string& input,appSettings& settings,aiProfile& profile,
    configManager& config,messageRouter& router) const
{
    commandOutput output;
    output.bWasCommand = true;

    const std::string prefix = "/profile ";
    std::string profileId = input.substr(prefix.length());

    if (profileId.empty())
    {
        output.bSuccess = false;
        output.output = "Profile name was empty. Example: /profile revia";
        output.reason = "Profile command missing profile name.";
        return output;
    }

    aiProfile newProfile;

    if (!config.LoadProfile(profileId, newProfile))
    {
        output.bSuccess = false;
        output.output = "Profile not found: " + profileId;
        output.reason = "Failed to load profile: " + profileId;
        return output;
    }

    profile = newProfile;
    settings.activeProfile = profileId;

    router.ApplyLLMSettings(settings.llm, profile);

    output.output = "Loaded profile: " + profile.displayName + " (" + profile.id + ")";
    return output;
}