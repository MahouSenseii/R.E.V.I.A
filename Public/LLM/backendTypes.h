#pragma once

#include <string>

enum class llmBackendType
{
    None,
    Placeholder,
    LLamaCpp,
};

enum class systemStatus
{
    Green,   // Good
    Yellow,  // Working but issues
    Red      // Failed
};

struct healthOutput
{
    bool bIsAvailable = false;

    systemStatus status = systemStatus::Red;

    std::string name;
    std::string message;
    std::string reason;
    int contextTokens = 0;
    int parallelSlots = 0;
    int responseTokenLimit = 0;
};
