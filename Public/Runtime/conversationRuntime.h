#pragma once

#include "Agents/turnCoordinator.h"
#include "Core/conversationContext.h"
#include "Core/logger.h"
#include "Core/messageRouter.h"
#include "Runtime/affectController.h"
#include "Runtime/runtimeEvents.h"
#include "Runtime/sessionResult.h"
#include "Speech/speechService.h"

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>

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

    ConversationRuntime(
        messageRouter& router,
        conversationContext& context,
        agents::TurnCoordinator& coordinator,
        speech::SpeechService& speech,
        AffectController& affect,
        RuntimeEventBus& events,
        logger& log,
        StateHandler stateHandler,
        AffectHandler affectHandler);

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

private:
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
    std::uint64_t turnCounter = 0;
    std::uint64_t utteranceCounter = 0;
};

} // namespace revia::runtime
