#pragma once

//
// Shared enum types for Revia.
//

enum class logSeverity
{
    Warning,
    Error
};

enum class memoryType
{
    ChatLog,
    ImportantMemory,
    Debug,
    System
};

enum class llmBackendType
{
    None,
    Placeholder,
    LLamaCpp,
    Ollama,
    OpenAI,
    LMStudio,
    CustomHttp,
    LlamaCpp
};

enum class systemStatus
{
    Green,   // Good
    Yellow,  // Working but issues
    Red      // Failed
};

enum class memoryImportance
{
    Low,
    Medium,
    High
};