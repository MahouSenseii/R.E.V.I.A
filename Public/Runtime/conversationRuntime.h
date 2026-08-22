#pragma once

#include "Actions/actionTypes.h"
#include "Agents/turnCoordinator.h"
#include "Agents/conversationQualityMonitor.h"
#include "Core/conversationContext.h"
#include "Core/logger.h"
#include "Core/messageRouter.h"
#include "Evaluation/conversationEvaluation.h"
#include "Runtime/affectController.h"
#include "Runtime/runtimeEvents.h"
#include "Runtime/sessionResult.h"
#include "Speech/speechService.h"

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::runtime
{

// Owns conversational turns and nothing else.
//
// ReviaSession decides lifecycle, commands, permissions, and whether an initiative cue
// is allowed to interrupt. Once a turn is approved, this component owns context,
// dialogue posture, generation, reply grounding, streaming speech, and turn telemetry.
class ConversationRuntime
{
public:
    using StateHandler = std::function<void(RuntimeState, const std::string&)>;
    using AffectHandler = std::function<void(const AffectSnapshot&)>;
    using InternetSettingsProvider =
        std::function<actions::CapabilitySettings::InternetAccess()>;
    using InternetLookupHandler =
        std::function<actions::ActionOutcome(const std::string&)>;

    ConversationRuntime(
        messageRouter& router,
        conversationContext& context,
        agents::TurnCoordinator& coordinator,
        speech::SpeechService& speech,
        AffectController& affect,
        RuntimeEventBus& events,
        logger& log,
        StateHandler stateHandler,
        AffectHandler affectHandler,
        InternetSettingsProvider internetSettingsProvider,
        InternetLookupHandler internetLookupHandler);

    SessionResult Reply(
        const std::string& input,
        const aiProfile& profile,
        bool llmAvailable,
        bool shouldSpeak,
        std::stop_token stopToken = {});

    // Generates an unprompted but evidence-grounded opening. The cue is never stored as
    // a user message and never enters automatic memory classification; only Revia's
    // visible line joins conversation history so a natural user reply has context.
    SessionResult StartConversation(
        const std::string& cue,
        const std::string& evidence,
        const aiProfile& profile,
        bool llmAvailable,
        bool shouldSpeak,
        std::stop_token stopToken = {});

    // Runs one conversation-contract evaluation turn against the active model.
    //
    // It uses the same posture assembly, style guidance, and turn coordinator a real
    // reply does, and deliberately none of the rest: an evaluation turn never enters
    // dialogue history, never reaches durable memory, never moves the response posture,
    // never speaks, and is scored by the caller rather than by the live quality counters.
    // A regression suite that shifted Revia's mood and filled her memory with test
    // prompts would be measuring a runtime it had already changed.
    [[nodiscard]] evaluation::EvaluationReply EvaluateTurn(
        const std::string& input,
        const std::vector<conversationMessage>& priorTurns,
        const aiProfile& profile,
        bool llmAvailable,
        std::stop_token stopToken = {});

    [[nodiscard]] agents::ConversationQualitySnapshot QualitySnapshot() const;

private:
    // The system posture a non-proactive turn is generated under. Shared with the
    // evaluation path so the suite exercises the prompt the user actually gets.
    [[nodiscard]] std::string BuildTurnPosture(
        const std::string& policyInput,
        const std::vector<conversationMessage>& promptContext,
        const aiProfile& profile,
        bool llmAvailable) const;
    SessionResult Generate(
        const std::string& policyInput,
        const std::vector<conversationMessage>& promptContext,
        const aiProfile& profile,
        bool llmAvailable,
        bool shouldSpeak,
        bool evaluateMemory,
        bool proactive,
        std::stop_token stopToken);
    void PublishComponent(
        const std::string& component,
        const std::string& phase,
        const std::string& message,
        double elapsedMilliseconds,
        int queueDepth,
        std::uint64_t turnId) const;

    messageRouter& router;
    conversationContext& context;
    agents::TurnCoordinator& coordinator;
    speech::SpeechService& speech;
    AffectController& affect;
    RuntimeEventBus& events;
    logger& log;
    StateHandler setState;
    AffectHandler publishAffect;
    InternetSettingsProvider internetSettings;
    InternetLookupHandler internetLookup;
    agents::ConversationQualityMonitor qualityMonitor;
    std::uint64_t turnCounter = 0;
    std::uint64_t utteranceCounter = 0;
};

} // namespace revia::runtime
