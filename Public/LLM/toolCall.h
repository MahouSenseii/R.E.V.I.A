#pragma once

#include <string>
#include <vector>

// Function-calling values shared by every language backend. They carry text only: the
// definitions come from the turn's tool registry, the calls are what the model asked
// for, and the results are what the host executed. None of them grants anything; a
// backend only transports them, and the tool loop decides what runs.
namespace revia::llm
{

// One function the model may call, in the OpenAI shape: a name, a sentence about when
// to use it, and a JSON Schema for its arguments.
struct ToolDefinition
{
    std::string name;
    std::string description;
    // Serialized JSON Schema object.
    std::string parametersSchema;
};

// One call the model asked for. `arguments` is the raw JSON text the model wrote, kept
// unparsed so validation happens in one place, against the registry's schema.
struct ToolCall
{
    std::string id;
    std::string name;
    std::string arguments;
};

// What the host returns for one call, already bounded and marked as untrusted data.
struct ToolResult
{
    std::string callId;
    std::string name;
    std::string content;
    bool error = false;
};

// One model step that asked for tools: the text it wrote beside the calls, the calls,
// and their results in the same order.
struct ToolRound
{
    std::string assistantText;
    std::vector<ToolCall> calls;
    std::vector<ToolResult> results;
};

// Everything a backend needs to offer tools on one request and replay the earlier
// steps of the same turn.
struct ToolExchange
{
    std::vector<ToolDefinition> definitions;
    std::vector<ToolRound> rounds;
    // False on the last step a budget allows: the tools stay described, because the
    // replayed calls refer to them, but the model is asked to answer in text.
    bool allowCalls = true;
};

} // namespace revia::llm
