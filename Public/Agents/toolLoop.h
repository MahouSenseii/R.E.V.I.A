#pragma once

#include "Agents/toolRegistry.h"
#include "Core/conversationMessage.h"
#include "Core/messageRouter.h"
#include "Intelligence/intelligenceTypes.h"
#include "LLM/privateMemoryAccess.h"
#include "LLM/responseTypes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::agents
{

struct ToolLoopLimits
{
    // Model requests per turn, the final answer included.
    std::size_t maxSteps = 8;
    // Wall-clock budget for the whole loop. Past it the next request asks for text.
    std::chrono::milliseconds budget{90000};
    std::size_t maxCallsPerStep = 4;
    // Each result, and all results of the turn together, as the model will read them.
    std::size_t maxResultBytes = 4000;
    std::size_t maxTranscriptBytes = 12000;
};

// One line for the chat's activity view: "Searching the web for X", then its outcome.
struct ToolActivity
{
    // "Running", "Ready" or "Error".
    std::string phase;
    std::string tool;
    std::string message;
    double elapsedMilliseconds = -1.0;
};

// What the loop records about every call it was asked to make, whether or not it ran.
// The action runtime keeps its own audit for actions; this covers every tool.
struct ToolAuditRecord
{
    std::uint64_t turnId = 0;
    std::string tool;
    std::string callId;
    // SHA-256 of the raw arguments, so the record proves what was asked without copying it.
    std::string argumentsDigest;
    bool executed = false;
    bool succeeded = false;
    std::string outcome;
};

// Runs one conversational answer with tools: generate, and while the model asks for
// tools, validate each call against its schema, execute it through its owner, append the
// call and its bounded, untrusted result, and generate again. Up to maxSteps requests,
// under a time and transcript budget. Text arrives through onDelta exactly as one reply
// would, so the caller's ReplyStreamGate governs every word shown, including text the
// model writes beside its calls.
class ToolLoop
{
  public:
    using ActivityHandler = std::function<void(const ToolActivity&)>;
    using AuditHandler = std::function<void(const ToolAuditRecord&)>;
    // Called once when the backend cannot take tools at all, before the loop answers
    // without them. The runtime uses it to restore its deterministic routers.
    using UnavailableHandler = std::function<void()>;
    using GeneratedMediaHandler = std::function<void(const std::vector<conversationMediaReference>&)>;

    explicit ToolLoop(ToolRegistry registry, ToolLoopLimits limits = {});

    void SetActivityHandler(ActivityHandler handler);
    void SetAuditHandler(AuditHandler handler);
    void SetUnavailableHandler(UnavailableHandler handler);
    // Receipts also reach the owner after revocation so it can retire unpublished artifacts.
    void SetGeneratedMediaHandler(GeneratedMediaHandler handler);
    // The turn's admission; checked before every request and every call.
    void SetAdmission(std::function<bool()> admission);
    void SetTurnId(std::uint64_t turnId);

    [[nodiscard]] bool Active() const;
    [[nodiscard]] const ToolRegistry& Registry() const;

    [[nodiscard]] responseOutput Run(const messageRouter& router, const std::string& input, const std::vector<conversationMessage>& context,
        std::stop_token stopToken, const messageRouter::DeltaHandler& onDelta, const intelligence::IntelligenceDecision& decision,
        llm::PrivateMemoryAccess memoryAccess) const;

    // A result as the model reads it: labelled as untrusted data, runtime markers
    // neutralized, and cut to `maximumBytes` on a character boundary.
    [[nodiscard]] static std::string FormatResult(const std::string& tool, const ToolExecution& execution, std::size_t maximumBytes);

  private:
    [[nodiscard]] std::vector<llm::ToolResult> Execute(
        const std::vector<llm::ToolCall>& calls, const ToolCallContext& context, std::size_t& transcriptBytes, std::size_t& executed) const;

    ToolRegistry registry;
    ToolLoopLimits limits;
    ActivityHandler activity;
    AuditHandler audit;
    UnavailableHandler unavailable;
    GeneratedMediaHandler generatedMedia;
    std::function<bool()> admission;
    std::uint64_t turnId = 0;
};

} // namespace revia::agents
