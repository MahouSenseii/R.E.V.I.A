#pragma once

#include "commandManager.h"
#include "configManager.h"
#include "Core/logger.h"
#include "Core/memoryManager.h"
#include "Core/messageRouter.h"
#include "Core/conversationContext.h"
#include "Library/enumLibrary.h"

class reviaApp
{
public:
    reviaApp();
    ~reviaApp();

    void Run();

private:

    // Later will move to a command manager but here now for testing
    void PrintStatus() const;

    logger appLogger;
    memoryManager memory;
    messageRouter router;
    configManager config;
    commandManager commands;
    conversationContext context;
    appSettings settings;
    aiProfile profile;

    bool bIsRunning = false;
};