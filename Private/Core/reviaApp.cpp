#include "Core/reviaApp.h"

#include <iostream>
#include <string>

using namespace std;

reviaApp::reviaApp() = default;

reviaApp::~reviaApp() = default;

void reviaApp::Run()
{
    bIsRunning = true;

    appLogger.Log("Starting core...");
    appLogger.Log("Type '/exit' to quit.");



    if (!config.LoadSettings(settings))
    {
        appLogger.Error("Failed to load settings. Exiting application.");
        return;
    }

    if (!config.LoadProfile(settings.activeProfile, profile))
    {
        appLogger.Error("Failed to load profile: " + settings.activeProfile);
        return;
    }

    router.ApplyLLMSettings(settings.llm, profile);

    if (!router.IsLLMAvailable())
    {
        appLogger.Warning("LLM backend is not available. Make sure llama.cpp server is running.");
    }
    else
    {
        appLogger.Log("LLM backend is available.");
    }

    appLogger.Log("Loaded profile: " + profile.displayName);

    string input;

    if (!appLogger.Check(bIsRunning, logSeverity::Error, "App failed to enter running state."))
    {
        return;
    }


    while (bIsRunning)
    {
        cout << "You: ";
        getline(cin, input);

        if (!appLogger.Check(cin.good(), logSeverity::Error, "Input stream failed while reading user input."))
        {
            bIsRunning = false;
            continue;
        }


        const commandOutput commandResult = commands.HandleCommand(input, settings, profile, config, router);

        if (commandResult.bWasCommand)
        {
            if (!commandResult.bSuccess)
            {
                appLogger.Warning(commandResult.reason);
            }

            if (!commandResult.output.empty())
            {
                cout << commandResult.output << "\n";
            }

            if (commandResult.bShouldExit)
            {
                bIsRunning = false;
            }

            continue;
        }

        if (memory.ShouldRemember(input))
        {
            if (!memory.SaveMemory(memoryType::ImportantMemory, "User", input))
            {
                appLogger.Warning("Failed to save important user memory.");
            }
        }

        context.AddMessage("user", input);

        const responseOutput output = router.RouteMessage(input, context.GetRecentMessages());

        if (!appLogger.Check(output.bSuccess, logSeverity::Warning, output.reason))
        {
            cout << "Revia: " << output.response << "\n\n";
            continue;
        }

        if (!appLogger.Check(!output.response.empty(), logSeverity::Warning, "Router returned an empty response."))
        {
            continue;
        }

        context.AddMessage("assistant", output.response);

        cout << "Revia: " << output.response << "\n\n";
    }

    appLogger.Log("Shutting down...");
}

void reviaApp::PrintStatus() const
{
    cout << "\n========== R.E.V.I.A Status ==========\n";

    cout << "Active Profile: " << settings.activeProfile << "\n";
    cout << "Profile Name:   " << profile.displayName << "\n";
    cout << "Profile ID:     " << profile.id << "\n";

    cout << "\nLLM Settings\n";
    cout << "Backend:        " << settings.llm.backend << "\n";
    cout << "Host:           " << settings.llm.host << "\n";
    cout << "Port:           " << settings.llm.port << "\n";
    cout << "Model Name:     " << settings.llm.modelName << "\n";
    cout << "Temperature:    " << settings.llm.temperature << "\n";
    cout << "Max Tokens:     " << settings.llm.maxTokens << "\n";

    cout << "\nProfile Overrides\n";
    cout << "Temperature Override: "
         << (profile.bHasTemperatureOverride ? "true" : "false") << "\n";

    cout << "Max Tokens Override:  "
         << (profile.bHasMaxTokensOverride ? "true" : "false") << "\n";

    if (profile.bHasTemperatureOverride)
    {
        cout << "Profile Temperature:  " << profile.temperature << "\n";
    }

    if (profile.bHasMaxTokensOverride)
    {
        cout << "Profile Max Tokens:   " << profile.maxTokens << "\n";
    }

    cout << "======================================\n\n";
}
