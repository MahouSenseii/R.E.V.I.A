#pragma once

#include "Actions/actionTypes.h"
#include "Agents/turnCoordinator.h"
#include "Agents/conversationQualityMonitor.h"
#include "Core/conversationContext.h"
#include "Core/logger.h"
#include "Core/messageRouter.h"
#include "Intelligence/humanizationState.h"
#include "Intelligence/intelligenceRouter.h"
#include "Intelligence/reflexRouter.h"
#include "Evaluation/conversationEvaluation.h"
#include "Emotion/emotionRuntime.h"
#include "Identity/relationshipState.h"
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
        std::function<actions::ActionOutcome(const std::string&, const std::string&)>;
    using ResponseFilterSettingsProvider = std::function<responseFilterSettings()>;
    using ScreenContextProvider = std::function<std::string()>;
    // Supplies the relationship with whoever is currently speaking. A provider rather
    // than a parameter because every call site would otherwise have to thread a speaker
    // through, and the session already owns who that is.
    using RelationshipProvider = std::function<identity::RelationshipState()>;
    // Supplies who Revia currently is. Appraisal scales by personality, so whatever
    // decides how an event feels needs the same development state the prompt shows.
    using DevelopmentProvider = std::function<identity::DevelopmentState()>;
    // Lets the session move drives from the same stimulus the appraisal saw, so wanting
    // and feeling cannot disagree about what happened.
    using StimulusObserver = std::function<void(const emotion::Stimulus&)>;
    // Captures the screen on demand for a turn that explicitly asked about it. Separate
    // from ScreenContextProvider, which only ever reads what ambient observation already
    // cached and returns nothing when that is off.
    using ScreenCaptureRequest = std::function<std::string()>;

    ConversationRuntime(
        messageRouter& router,
        conversationContext& context,
        agents::TurnCoordinator& coordinator,
        speech::SpeechService& speech,
        AffectController& affect,
        emotion::EmotionRuntime& emotions,
        RuntimeEventBus& events,
        logger& log,
        StateHandler stateHandler,
        AffectHandler affectHandler,
        InternetSettingsProvider internetSettingsProvider,
        InternetLookupHandler internetLookupHandler,
        ResponseFilterSettingsProvider responseFilterSettingsProvider,
        ScreenContextProvider screenContextProvider,
        RelationshipProvider relationshipProvider = {},
        DevelopmentProvider developmentProvider = {},
        StimulusObserver stimulusObserver = {},
        ScreenCaptureRequest screenCaptureRequest = {});

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

    SessionResult StartCuriosityConversation(
        const std::string& topic,
        const std::string& rationale,
        const std::string& researchGrounding,
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
    [[nodiscard]] agents::ResponseFilterContext BuildResponseFilterContext(
        const std::string& policyInput,
        const std::vector<conversationMessage>& promptContext) const;
    SessionResult Generate(
        const std::string& policyInput,
        const std::vector<conversationMessage>& promptContext,
        const aiProfile& profile,
        bool llmAvailable,
        bool shouldSpeak,
        bool evaluateMemory,
        bool proactive,
        const std::string& proactiveInstruction,
        const std::string& precomputedInternetGrounding,
        std::stop_token stopToken);
    void PublishComponent(
        const std::string& component,
        const std::string& phase,
        const std::string& message,
        double elapsedMilliseconds,
        int queueDepth,
        std::uint64_t turnId) const;
    void PublishInternetActivity(
        const std::string& phase,
        const std::string& query,
        const std::string& provider,
        const std::string& detail,
        double elapsedMilliseconds,
        int sourceCount,
        std::uint64_t turnId) const;

    messageRouter& router;
    conversationContext& context;
    agents::TurnCoordinator& coordinator;
    speech::SpeechService& speech;
    AffectController& affect;
    // Primary. AffectController stays as the deterministic fallback and baseline, but
    // what reaches the prompt now comes from appraisal.
    emotion::EmotionRuntime& emotions;
    RuntimeEventBus& events;
    logger& log;
    StateHandler setState;
    AffectHandler publishAffect;
    InternetSettingsProvider internetSettings;
    InternetLookupHandler internetLookup;
    ResponseFilterSettingsProvider filterSettingsProvider;
    ScreenContextProvider screenContextProvider;
    RelationshipProvider relationshipProvider;
    DevelopmentProvider developmentProvider;
    StimulusObserver stimulusObserver;
    ScreenCaptureRequest screenCaptureRequest;
    agents::ConversationQualityMonitor qualityMonitor;
    intelligence::HumanizationController humanization;
    intelligence::IntelligenceRouter intelligenceRouter;
    intelligence::ReflexRouter reflexRouter;
    std::string previousReflexResponse;
    std::string previousReflexInput;
    std::size_t repeatedReflexCalls = 0;
    std::uint64_t turnCounter = 0;
    std::uint64_t utteranceCounter = 0;
};

} // namespace revia::runtime
