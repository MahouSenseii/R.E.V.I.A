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

    while (bIsRunning)
    {
        cout << "You: ";
        getline(cin, input);

        if (!appLogger.Check(cin.good(), logSeverity::Error, "Input stream failed while reading user input."))
        {
            bIsRunning = false;
            continue;
        }

        if (router.IsExitCommand(input))
        {
            cout << "Exiting R.E.V.I.A...\n";
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

        if (profile.bMemoryEnabled && memory.ShouldRemember(input))
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

        // Only print if streaming didn't already output the tokens live
        if (!output.bWasStreamed)
        {
            cout << "Revia: " << output.response << "\n\n";
        }

        context.AddMessage("assistant", output.response);
    }

    appLogger.Log("Shutting down...");
}
