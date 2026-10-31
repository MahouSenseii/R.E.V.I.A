#pragma once

#include "LLM/providerExchange.h"
#include "LLM/toolCall.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct latencySample
{
    std::string stage;
    double milliseconds = 0.0;
    bool bAggregate = false;
    // Empty for a duration. Otherwise the sample is a count in this unit (for example
    // "tok" for the backend's cached prompt tokens), held in `milliseconds` so the timing
    // line can show it beside the stage it explains.
    std::string unit{};
};

// Prompt spans use character counts; llama.cpp supplies the actual token total.
// Stable spans can share a cached prefix; a changed span invalidates everything after it.
struct promptSection
{
    std::string name;
    std::size_t characters = 0;
    bool stable = false;
};

struct promptFitReport
{
    bool available = false;
    bool backendCounted = false;
    std::size_t promptTokens = 0;
    std::size_t contextTokens = 0;
    std::size_t responseReserve = 0;
    std::size_t templateReserve = 0;
    std::size_t inputMessages = 0;
    std::size_t retainedMessages = 0;
    std::string reason;
};

struct responseOutput
{
    bool bSuccess = false;
    // A background request that yielded to interactive inference before it produced
    // anything. It made no progress, so its owner may wait and ask again.
    bool bPreempted = false;
    bool bShouldRemember = false;
    bool bShouldSpeak = true;
    bool bWasStreamed = false;  // true when visible deltas were delivered to a consumer

    std::string response;
    // Model output before style repair, retained separately from the delivered reply.
    std::string rawResponse;
    std::string reason;
    // Hidden <think> content, retained for diagnostics and excluded from replies and speech.
    std::string reasoning;
    // Deterministic filtering always runs; optional AI review precedes speech, memory and history.
    bool bHardFilterChanged = false;
    // True when unsafe or ungrounded output was replaced, rather than cosmetically repaired.
    // Routing treats replacement as evidence that the next turn needs more effort.
    bool bHardFilterBlocked = false;
    bool bAiFilterReviewed = false;
    bool bAiFilterChanged = false;
    std::string filterSummary;
    // Pre-generation routing telemetry. Strings keep this transport structure independent
    // of the router implementation while still making every fallback auditable.
    std::string requestedTier;
    std::string selectedTier;
    std::string selectedModel;
    std::string reasoningMode;
    std::string routingReason;
    float routingConfidence = 0.0F;
    bool bRoutingFallback = false;
    std::string routingFallbackReason;
    std::vector<latencySample> timings;
    // How the prompt that produced this reply was made up, in prompt order.
    std::vector<promptSection> promptSections;
    // Usage is unknown unless bTokensReported is true; zero does not imply a free request.
    // When usage is absent, callers must bound work by another limit.
    std::uint32_t promptTokens = 0;
    std::uint32_t completionTokens = 0;
    bool bTokensReported = false;
    promptFitReport contextFit;
    // Calls the model asked for on a request that offered tools. Nothing has run them:
    // the tool loop validates and executes them, or the reply simply ends.
    std::vector<revia::llm::ToolCall> toolCalls;
    // The backend refused a request because it offered tools (a server without tool
    // support). The caller falls back to a request without them.
    bool bToolsRejected = false;
    // Tool calls the host actually executed for this reply, across every step.
    std::size_t toolCallsExecuted = 0;
    // The exact local request and raw output behind a delivered text reply, so an owner
    // judgment can be bound to it. Shared and immutable: copying the reply does not copy it.
    std::shared_ptr<const revia::llm::ProviderExchange> exchange;

    [[nodiscard]] std::uint32_t TotalTokens() const
    {
        return promptTokens + completionTokens;
    }
};
