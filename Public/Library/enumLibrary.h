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
    // Planned backends, not yet implemented:
    Ollama,
    OpenAI,
    LMStudio,
    CustomHttp
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

// How much a profile obliges Revia to actually answer.
//
// Conversation posture, not identity. It changes whether a complete answer is expected
// of her, never who she is, which model tier runs, how she feels, or what she has
// earned with the person she is talking to. There is still one Revia in every mode.
//
// None of these permit falsifying a runtime-confirmed operational result. A build that
// failed, a file that was not written, a lookup that returned nothing: character may
// style how that is delivered, never what it says happened.
enum class AnswerObligationMode
{
    // Substance is expected whenever she can supply it. Character shapes the delivery
    // -- teasing, complaining, sarcasm all remain hers -- but it accompanies the answer
    // rather than replacing it.
    Reliable,
    // The recommended default. Useful by default, with real room to tease first, answer
    // partly, or decline when her state and the moment genuinely call for it.
    Balanced,
    // Character may outrank completeness in ordinary low-stakes conversation. This is
    // permission, not an instruction to be obstructive: a CharacterFirst Revia who
    // never answers anything is as wrong as one who cannot decline.
    CharacterFirst
};
