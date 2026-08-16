#include "Core/reviaApp.h"
#include "Core/localApiKey.h"

#include <iostream>
#include <string>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

using namespace std;

namespace
{
    double ElapsedMilliseconds(const std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }
}

reviaApp::reviaApp() = default;

reviaApp::~reviaApp() = default;

void reviaApp::Run()
{
    const auto startupStarted = std::chrono::steady_clock::now();
    std::vector<latencySample> startupTimings;
    bIsRunning = true;

    appLogger.Log("Starting core...");
    appLogger.Log("Type '/exit' to quit.");

    auto stageStarted = std::chrono::steady_clock::now();
    const bool bSettingsLoaded = config.LoadSettings(settings);
    startupTimings.push_back({"settings_load", ElapsedMilliseconds(stageStarted)});
    if (!bSettingsLoaded)
    {
        appLogger.Error("Failed to load settings. Exiting application.");
        startupTimings.push_back({"startup_total", ElapsedMilliseconds(startupStarted), true});
        appLogger.Timing("startup", startupTimings);
        return;
    }
    if (settings.llm.apiKey.empty())
    {
        settings.llm.apiKey = revia::core::GenerateLocalApiKey();
    }
    if (settings.embedding.apiKey.empty())
    {
        settings.embedding.apiKey = revia::core::GenerateLocalApiKey();
    }

    stageStarted = std::chrono::steady_clock::now();
    const bool bProfileLoaded = config.LoadProfile(settings.activeProfile, profile);
    startupTimings.push_back({"profile_load", ElapsedMilliseconds(stageStarted)});
    if (!bProfileLoaded)
    {
        appLogger.Error("Failed to load profile: " + settings.activeProfile);
        startupTimings.push_back({"startup_total", ElapsedMilliseconds(startupStarted), true});
        appLogger.Timing("startup", startupTimings);
        return;
    }

    std::string actionError;
    stageStarted = std::chrono::steady_clock::now();
    if (!actionRuntime.Initialize("Config/capabilities.json", "Audit/actions.jsonl", actionError))
    {
        appLogger.Warning("Action runtime disabled: " + actionError);
    }
    else
    {
        appLogger.Log("Capability runtime initialized. Use /capabilities to inspect its scope.");
    }
    startupTimings.push_back({"action_runtime_init", ElapsedMilliseconds(stageStarted)});

    stageStarted = std::chrono::steady_clock::now();
    router.ApplyLLMSettings(settings.llm, settings.embedding, profile);
    startupTimings.push_back({"llm_configuration", ElapsedMilliseconds(stageStarted)});

    stageStarted = std::chrono::steady_clock::now();
    const bool bIsLLMAvailable = EnsureLLMAvailable();
    startupTimings.push_back({"llm_health_or_start", ElapsedMilliseconds(stageStarted)});
    stageStarted = std::chrono::steady_clock::now();
    const bool bIsEmbeddingAvailable = EnsureEmbeddingAvailable();
    startupTimings.push_back({"embedding_health_or_start", ElapsedMilliseconds(stageStarted)});
    if (bIsEmbeddingAvailable && profile.bMemoryEnabled)
    {
        stageStarted = std::chrono::steady_clock::now();
        turnCoordinator.BackfillMemoryEmbeddings(router, settings.embedding.modelName);
        startupTimings.push_back({"embedding_backfill_queue", ElapsedMilliseconds(stageStarted)});
    }

    appLogger.Log("Loaded profile: " + profile.displayName);
    startupTimings.push_back({"startup_total", ElapsedMilliseconds(startupStarted), true});
    appLogger.Timing("startup", startupTimings);

    if (bIsLLMAvailable)
    {
        cout << profile.displayName << ": Hi. I'm online.\n\n";
    }

    string input;

    while (bIsRunning)
    {
        PrintMemoryAgentEvents();
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

        if (TryHandleActionInput(input))
        {
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

        context.AddMessage("user", input);

        const std::uint64_t currentTurn = ++turnCounter;
        const auto turnStarted = std::chrono::steady_clock::now();
        const revia::agents::TurnAgentResult turnResult = turnCoordinator.Execute(
            router,
            input,
            context.GetRecentMessages(),
            profile.bMemoryEnabled,
            currentTurn);
        const responseOutput& output = turnResult.response;
        std::vector<latencySample> turnTimings = output.timings;
        turnTimings.push_back({"turn_total", ElapsedMilliseconds(turnStarted), true});
        appLogger.Timing("turn #" + std::to_string(currentTurn), turnTimings);

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

        PrintMemoryAgentEvents();
    }

    const auto shutdownStarted = std::chrono::steady_clock::now();
    std::vector<latencySample> shutdownTimings;
    appLogger.Log("Shutting down...");
    stageStarted = std::chrono::steady_clock::now();
    turnCoordinator.Stop();
    shutdownTimings.push_back({"memory_agent_stop", ElapsedMilliseconds(stageStarted)});
    PrintMemoryAgentEvents();
    if (settings.llm.bShutdownServerOnExit && llamaServerProcess.WasStartedByRevia())
    {
        appLogger.Log("Stopping the llama.cpp process owned by Revia...");
        stageStarted = std::chrono::steady_clock::now();
        llamaServerProcess.Stop();
        shutdownTimings.push_back({"llm_server_stop", ElapsedMilliseconds(stageStarted)});
    }
    if (settings.embedding.bShutdownServerOnExit &&
        embeddingServerProcess.WasStartedByRevia())
    {
        appLogger.Log("Stopping the embedding process owned by Revia...");
        stageStarted = std::chrono::steady_clock::now();
        embeddingServerProcess.Stop();
        shutdownTimings.push_back({"embedding_server_stop", ElapsedMilliseconds(stageStarted)});
    }
    shutdownTimings.push_back({"shutdown_total", ElapsedMilliseconds(shutdownStarted), true});
    appLogger.Timing("shutdown", shutdownTimings);
    appLogger.Log("Shutdown complete.");
}

void reviaApp::PrintMemoryAgentEvents()
{
    for (const revia::agents::MemoryAgentEvent& event : turnCoordinator.DrainMemoryEvents())
    {
        const std::string timingScope = event.operation == "memory_backfill"
            ? "memory backfill"
            : "memory turn #" + std::to_string(event.turnId);
        appLogger.Timing(timingScope, event.decision.timings);

        if (event.operation == "memory_backfill")
        {
            if (!event.decision.bSuccess)
            {
                appLogger.Warning("Memory embedding backfill failed: " + event.decision.reason);
            }
            else if (!event.saveSucceeded)
            {
                appLogger.Warning("Failed to save a backfilled memory embedding.");
            }
        }
        else if (!event.decision.bSuccess)
        {
            appLogger.Warning("Automatic memory evaluation failed: " + event.decision.reason);
        }
        else if (event.decision.bShouldRemember && !event.saveSucceeded)
        {
            appLogger.Warning("Failed to save automatic memory.");
        }
        else if (event.wasAdded)
        {
            cout << "[Memory] Remembered: " << event.decision.summary << "\n\n";
        }
    }
}

bool reviaApp::EnsureLLMAvailable()
{
    const healthOutput initialHealth = router.CheckLLMHealth();
    if (initialHealth.bIsAvailable)
    {
        appLogger.Log("LLM backend is available.");
        return true;
    }

    if (initialHealth.status == systemStatus::Yellow)
    {
        appLogger.Warning(initialHealth.reason);
        return false;
    }

    if (settings.llm.backend != "LLamaCpp")
    {
        appLogger.Warning("The configured LLM backend is unavailable; llama.cpp auto-start was not attempted.");
        return false;
    }

    if (!settings.llm.bAutoStartServer)
    {
        appLogger.Warning("LLM backend is not available and automatic startup is disabled.");
        return false;
    }

    appLogger.Log("LLM backend is offline. Starting llama.cpp...");
    std::string launchError;
    if (!llamaServerProcess.Start(settings.llm, launchError))
    {
        appLogger.Warning(launchError);
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(settings.llm.startupTimeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!llamaServerProcess.IsRunning())
        {
            appLogger.Warning(
                "llama.cpp exited before becoming ready. Check Logs/llama-server.stderr.log.");
            llamaServerProcess.Stop();
            return false;
        }

        if (router.IsLLMAvailable())
        {
            appLogger.Log("llama.cpp started and the LLM backend is available.");
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    appLogger.Warning(
        "Timed out waiting for llama.cpp to become ready. Check Logs/llama-server.stderr.log.");
    llamaServerProcess.Stop();
    return false;
}

bool reviaApp::EnsureEmbeddingAvailable()
{
    if (!settings.embedding.bEnabled)
    {
        appLogger.Log("Semantic memory is disabled; SQLite FTS retrieval remains available.");
        return false;
    }

    const healthOutput initialHealth = router.CheckEmbeddingHealth();
    if (initialHealth.bIsAvailable)
    {
        appLogger.Log("Semantic-memory embeddings are available.");
        return true;
    }

    if (initialHealth.status == systemStatus::Yellow)
    {
        appLogger.Warning(initialHealth.reason + " Falling back to SQLite FTS retrieval.");
        return false;
    }
    if (!settings.embedding.bAutoStartServer)
    {
        appLogger.Warning(
            "The embedding server is offline and automatic startup is disabled. "
            "Falling back to SQLite FTS retrieval.");
        return false;
    }

    appLogger.Log("Embedding backend is offline. Starting dedicated llama.cpp embeddings...");
    std::string launchError;
    if (!embeddingServerProcess.StartEmbedding(settings.embedding, launchError))
    {
        appLogger.Warning(launchError + " Falling back to SQLite FTS retrieval.");
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(settings.embedding.startupTimeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!embeddingServerProcess.IsRunning())
        {
            appLogger.Warning(
                "The embedding server exited before becoming ready. "
                "Check Logs/embedding-server.stderr.log.");
            embeddingServerProcess.Stop();
            return false;
        }

        if (router.CheckEmbeddingHealth().bIsAvailable)
        {
            appLogger.Log("Dedicated semantic-memory embeddings are available.");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    appLogger.Warning(
        "Timed out waiting for the embedding server. "
        "Check Logs/embedding-server.stderr.log; SQLite FTS fallback remains active.");
    embeddingServerProcess.Stop();
    return false;
}

bool reviaApp::TryHandleActionInput(const std::string& input)
{
    if (input == "/capabilities")
    {
        cout << actionRuntime.StatusJson() << "\n";
        return true;
    }

    if (input.rfind("/plan ", 0) == 0)
    {
        if (!actionRuntime.IsInitialized())
        {
            cout << "Action runtime is not initialized.\n";
            return true;
        }

        const responseOutput proposal = router.PlanAction(input.substr(6));
        if (!proposal.bSuccess)
        {
            appLogger.Warning(proposal.reason);
            cout << "Revia: " << proposal.response << "\n";
            return true;
        }

        cout << "Planner proposal: " << proposal.response << "\n";
        auto parsed = actionRuntime.ParseJson(proposal.response);
        if (!parsed.succeeded)
        {
            appLogger.Warning(parsed.error);
            cout << "Action proposal rejected: " << parsed.error << "\n";
            return true;
        }
        parsed.request.requestedBy = "llm";
        ExecuteAction(std::move(parsed.request));
        return true;
    }

    auto parsed = actionRuntime.ParseCommand(input);
    if (!parsed.recognized)
    {
        return false;
    }
    if (!parsed.succeeded)
    {
        cout << "Action command rejected: " << parsed.error << "\n";
        return true;
    }

    ExecuteAction(std::move(parsed.request));
    return true;
}

void reviaApp::ExecuteAction(revia::actions::ActionRequest request)
{
    if (!actionRuntime.IsInitialized())
    {
        cout << "Action runtime is not initialized.\n";
        return;
    }

    const auto decision = actionRuntime.Evaluate(request);
    cout << "Action: " << revia::actions::ToString(request.type) << "\n";
    cout << "Source: " << revia::actions::PathToUtf8(request.source) << "\n";
    if (!request.destination.empty())
    {
        cout << "Destination: " << revia::actions::PathToUtf8(request.destination) << "\n";
    }
    cout << "Policy: " << revia::actions::ToString(decision.verdict)
         << " (" << decision.reason << ")\n";

    bool confirmed = false;
    if (decision.verdict == revia::actions::PolicyVerdict::RequiresConfirmation)
    {
        cout << "Allow this action? [y/N]: ";
        std::string answer;
        getline(cin, answer);
        std::transform(answer.begin(), answer.end(), answer.begin(), [](unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        });
        confirmed = answer == "y" || answer == "yes";
    }

    const auto outcome = actionRuntime.Execute(request, confirmed);
    PrintActionOutcome(outcome);
}

void reviaApp::PrintActionOutcome(const revia::actions::ActionOutcome& outcome)
{
    cout << (outcome.result.succeeded ? "Action succeeded: " : "Action stopped: ")
         << outcome.result.message << "\n";
    for (const auto& entry : outcome.result.entries)
    {
        cout << "  " << entry << "\n";
    }
    if (!outcome.result.content.empty())
    {
        cout << "----- file content -----\n"
             << outcome.result.content
             << "\n----- end content -----\n";
    }
    cout << "\n";
}
