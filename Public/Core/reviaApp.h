#pragma once
#include "Agents/turnCoordinator.h"

#include "Core/commandManager.h"
#include "Core/configManager.h"
#include "Core/logger.h"
#include "Core/messageRouter.h"
#include "Core/conversationContext.h"
#include "Library/enumLibrary.h"
#include "Actions/actionRuntime.h"
#include "LLM/LLamaCPP/llamaCppServerProcess.h"

#include <string>
#include <cstdint>

class reviaApp
{
public:
    reviaApp();
    ~reviaApp();

    void Run();

private:

    bool TryHandleActionInput(const std::string& input);
    bool EnsureLLMAvailable();
    bool EnsureEmbeddingAvailable();
    void PrintMemoryAgentEvents();
    void ExecuteAction(revia::actions::ActionRequest request);
    static void PrintActionOutcome(const revia::actions::ActionOutcome& outcome);

    logger appLogger;
    messageRouter router;
    configManager config;
    commandManager commands;
    conversationContext context;
    appSettings settings;
    aiProfile profile;
    revia::actions::ActionRuntime actionRuntime;
    llamaCppServerProcess llamaServerProcess;
    llamaCppServerProcess embeddingServerProcess;
    revia::agents::TurnCoordinator turnCoordinator;

    bool bIsRunning = false;
    std::uint64_t turnCounter = 0;
};
