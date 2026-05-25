#pragma once

#include "Core/commandManager.h"
#include "Core/configManager.h"
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
