#pragma once

#include <string>

#include "enumLibrary.h"

struct responseOutput
{
    bool bSuccess = false;
    bool bShouldRemember = false;
    bool bShouldSpeak = true;

    std::string response;
    std::string reason;
};

struct llmSettings
{
    std::string backend = "LLamaCpp";
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string modelName = "local-model";

    float temperature = 0.7f;
    int maxTokens = 512;
};

struct aiProfile
{
    std::string id = "assistant";
    std::string displayName = "Assistant";
    std::string systemPrompt = "You are a helpful local AI assistant.";

    // H3: when false, user input is not written to long-term memory.
    bool bMemoryEnabled = true;

    bool bHasTemperatureOverride = false;
    bool bHasMaxTokensOverride = false;

    float temperature = 0.7f;
    int maxTokens = 512;
};

struct appSettings
{
    std::string activeProfile = "assistant";
    llmSettings llm;
};

struct commandOutput
{
    bool bWasCommand = false;
    bool bShouldExit = false;
    bool bSuccess = true;

    std::string output;
    std::string reason;
};

struct conversationMessage
{
    std::string role;
    std::string content;
};

struct healthOutput
{
    bool bIsAvailable = false;

    systemStatus status = systemStatus::Red;

    std::string name;
    std::string message;
    std::string reason;
};

struct memoryEntry
{
    std::string speaker;
    std::string content;
    std::string source;

    memoryImportance importance = memoryImportance::Medium;
};
