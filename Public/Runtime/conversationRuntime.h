#pragma once

#include "Agents/responseFilterSettings.h"
#include "Core/conversationMessage.h"
#include "Core/profile.h"
#include "Actions/actionTypes.h"
#include "Agents/investigation.h"
#include "Agents/investigationAgent.h"
#include "Agents/turnCoordinator.h"
#include "Agents/conversationQualityMonitor.h"
#include "Agents/selfInquiry.h"
#include "Core/conversationContext.h"
#include "Core/logger.h"
#include "Core/messageRouter.h"
#include "Intelligence/humanizationState.h"
#include "Intelligence/intelligenceRouter.h"
#include "Intelligence/reflexRouter.h"
#include "Evaluation/conversationEvaluation.h"
#include "Memory/conversationRecall.h"
#include "Emotion/emotionRuntime.h"
#include "Identity/preferenceState.h"
#include "Identity/relationshipState.h"
#include "Runtime/affectController.h"
#include "Runtime/runtimeEvents.h"
#include "Runtime/responseLatency.h"
#include "Runtime/sessionResult.h"
#include "Identity/socialIdentity.h"
#include "Speech/speechService.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::runtime
{

// Runtime evidence used to route an admitted conversation turn.
struct RoutingInputs
{
    std::string input;
    // Proactive openings bypass request routing.
    bool proactive = false;
    bool allowScreenContext = true;
    bool allowInternetLookup = true;
    // Public turns keep separate continuity.
    bool publicAudience = false;
    // The runtime already fetched research for this turn.
    bool groundingAlreadyRetrieved = false;
    std::size_t recentContextCharacters = 0;
    std::optional<intelligence::IntelligenceTier> previousDeliveredTier;
    bool previousTurnWasUnreliable = false;
};

[[nodiscard]] intelligence::RoutingContext BuildRoutingContext(const RoutingInputs& inputs);
[[nodiscard]] std::optional<intelligence::IntelligenceTier> DeliveredResponseTier(const responseOutput& output);

// Owns admitted conversational turns; lifecycle and permissions stay with the session.
class ConversationRuntime
{
public:
    using StateHandler = std::function<void(RuntimeState, const std::string&)>;
    using AffectHandler = std::function<void(const AffectSnapshot&)>;
    using InternetSettingsProvider =
        std::function<actions::CapabilitySettings::InternetAccess()>;
    // Reads current desktop permissions per turn.
    using DesktopSettingsProvider =
        std::function<actions::CapabilitySettings::DesktopControl()>;
    using InternetLookupHandler =
        std::function<actions::ActionOutcome(const std::string&, const std::string&)>;
    using ResponseFilterSettingsProvider = std::function<responseFilterSettings()>;
    // Reads live self-inquiry limits on the next turn.
    using SelfInquirySettingsProvider = std::function<agents::SelfInquiryLimits()>;
    using ScreenContextProvider = std::function<std::string()>;
    // Reads the relationship for the current speaker.
    using RelationshipProvider = std::function<identity::RelationshipState()>;
    // Keeps appraisal and prompts aligned with current development.
    using DevelopmentProvider = std::function<identity::DevelopmentState()>;
    // Supplies the opinions worth showing this turn, already ranked and bounded. A
    // provider rather than stored state: what she likes changes between turns, and a
    // copy taken at construction would go stale the first time evidence moved one.
    using PreferenceProvider = std::function<std::vector<identity::Preference>()>;
    // What she currently wants and what she is in the middle of, already in words.
    //
    // A provider for the same reason the preference one is: both change between turns,
    // and a copy taken at construction would describe a Revia who wanted something
    // this morning. Prose rather than typed state so this header does not have to
    // depend on the autonomy domain to describe it.
    struct AutonomyContext
    {
        std::string wanting;
        std::string currentActivity;
        std::string backgroundTask;
        std::string finishedTask;
        std::string reminders;
    };
    using AutonomyContextProvider = std::function<AutonomyContext()>;
    // Lets the session move drives from the same stimulus the appraisal saw, so wanting
    // and feeling cannot disagree about what happened.
    using StimulusObserver = std::function<void(const emotion::Stimulus&)>;
    // Captures the screen on demand for a turn that explicitly asked about it. Separate
    // from ScreenContextProvider, which only ever reads what ambient observation already
    // cached and returns nothing when that is off.
    using ScreenCaptureRequest = std::function<std::string()>;
    // Consults the durable conversation archive for one turn that asked about what was
    // actually said, and returns the bounded block to ground the answer with. The
    // session owns the archive; this runtime owns the decision to ask. Returning a
    // rendered string rather than turns keeps the transcript itself out of the
    // conversational path except as the one block that reaches the prompt.
    //
    // The second argument is the question being answered. It is archived before the
    // reply is generated, so without it a search for "what did I say about X" reliably
    // finds the user asking what they said about X.
    using ConversationRecallHandler =
        std::function<std::string(const memory::RecallRequest&, const std::string& currentInput, const memory::MemoryScope& scope)>;
    // One sentence on what she can sing right now, from the song library. Without it she
    // answered "can you sing?" from the model's guess -- that she is text and cannot --
    // while a folder of songs sat ready to play.
    using SongListProvider = std::function<std::string()>;

    ConversationRuntime(messageRouter& router, conversationContext& context, agents::TurnCoordinator& coordinator,
        speech::SpeechService& speech, AffectController& affect, emotion::EmotionRuntime& emotions, RuntimeEventBus& events, logger& log,
        StateHandler stateHandler, AffectHandler affectHandler, InternetSettingsProvider internetSettingsProvider,
        DesktopSettingsProvider desktopSettingsProvider, InternetLookupHandler internetLookupHandler,
        ResponseFilterSettingsProvider responseFilterSettingsProvider, ScreenContextProvider screenContextProvider,
        RelationshipProvider relationshipProvider = {}, DevelopmentProvider developmentProvider = {},
        StimulusObserver stimulusObserver = {}, ScreenCaptureRequest screenCaptureRequest = {}, PreferenceProvider preferenceProvider = {},
        SelfInquirySettingsProvider selfInquirySettingsProvider = {}, ConversationRecallHandler conversationRecallHandler = {},
        AutonomyContextProvider autonomyContextProvider = {});
    ~ConversationRuntime();
    void ResetResponseLatency(const RuntimeStamp& origin);
    [[nodiscard]] ResponseLatencySnapshot ResponseLatencies() const;

    // Set once at startup, before the first turn.
    void SetSongListProvider(SongListProvider provider);
    void ResetParticipantContinuity();
    void SetPrivateAdmissionFactory(std::function<std::function<bool()>()> factory);
    using InvestigationExecutorFactory = std::function<agents::CheckExecutor(std::function<bool()>, std::stop_token)>;
    void SetInvestigationExecutorFactory(InvestigationExecutorFactory factory);

    // `turnReference` is added to this turn's context only, never to its history: text
    // the runtime fetched for the question, such as what the user copied.
    SessionResult Reply(const std::string& input, const aiProfile& profile, bool llmAvailable, bool shouldSpeak,
        std::stop_token stopToken = {}, const std::string& turnReference = {});

    // Public integrations get Revia's identity and the supplied channel history, but
    // never inherit the local user's dialogue, compressed history, durable memories,
    // screen/camera observations, or automatic web lookup. The caller supplies the
    // already-resolved viewer relationship so speaker identity cannot lag by one turn.
    SessionResult ReplyPublic(const std::string& input, const std::vector<conversationMessage>& channelHistory,
        const std::string& publicInstruction, const identity::RelationshipState& relationship, const aiProfile& profile, bool llmAvailable,
        bool shouldSpeak, std::stop_token stopToken = {});
    SessionResult ReplyForAudience(const std::string& input, const std::vector<conversationMessage>& channelHistory,
        const identity::AudienceContext& audience, const identity::RelationshipState& relationship, const aiProfile& profile,
        bool llmAvailable, bool shouldSpeak, std::stop_token stopToken = {}, std::function<bool()> admission = {},
        const std::string& turnReference = {}, std::chrono::steady_clock::time_point acceptedAt = {},
        const memory::MemoryScope& memoryScope = {});

    // Guest overload: the caller owns an isolated router with PublicGuestProfile(),
    // never the desktop router. No instance state, provider, logger or event bus is
    // read or mutated. Returns final filtered text only, without model diagnostics.
    [[nodiscard]] static aiProfile PublicGuestProfile();
    [[nodiscard]] static SessionResult ReplyPublic(messageRouter& isolatedRouter, const std::string& input,
        const std::vector<conversationMessage>& guestHistory, std::stop_token stopToken);

    // Generates an unprompted but evidence-grounded opening. The cue is never stored as
    // a user message and never enters automatic memory classification; only Revia's
    // visible line joins conversation history so a natural user reply has context.
    SessionResult StartConversation(const std::string& cue, const std::string& evidence, const aiProfile& profile, bool llmAvailable,
        bool shouldSpeak, std::stop_token stopToken = {}, std::uint64_t audienceRevision = 0, std::function<bool()> admission = {});

    SessionResult StartCuriosityConversation(const std::string& topic, const std::string& rationale, const std::string& researchGrounding,
        const aiProfile& profile, bool llmAvailable, bool shouldSpeak, std::stop_token stopToken = {}, std::uint64_t audienceRevision = 0,
        std::function<bool()> admission = {});

    // Runs one conversation-contract evaluation turn against the active model.
    //
    // It uses the same posture assembly, style guidance, and turn coordinator a real
    // reply does, and deliberately none of the rest: an evaluation turn never enters
    // dialogue history, never reaches durable memory, never moves the response posture,
    // never speaks, and is scored by the caller rather than by the live quality counters.
    // A regression suite that shifted Revia's mood and filled her memory with test
    // prompts would be measuring a runtime it had already changed.
    [[nodiscard]] evaluation::EvaluationReply EvaluateTurn(const std::string& input, const std::vector<conversationMessage>& priorTurns,
        const aiProfile& profile, bool llmAvailable, std::stop_token stopToken = {});

    [[nodiscard]] agents::ConversationQualitySnapshot QualitySnapshot() const;

private:
    struct TurnPolicy
    {
        bool publicAudience = false;
        bool allowScreenContext = true;
        bool allowInternetLookup = true;
        bool allowSelfInquiry = true;
        bool includePrivateHistory = true;
        std::optional<identity::RelationshipState> relationship;
        std::string instruction;
        std::function<bool()> deliveryAdmission;
        std::chrono::steady_clock::time_point acceptedAt{};
        std::uint64_t audienceRevision = 0;
        memory::MemoryScope memoryScope;
    };

    // Canonical state and posture for replies, proactive openings and evaluation.
    // Event/research instructions extend it; audience policy controls private history.
    // What she is made of -- where she runs, how she thinks, hears, and speaks -- from
    // the live speech state. Shared by the turn posture and the self-inquiry so the two
    // cannot describe her body differently.
    [[nodiscard]] std::string DescribeBody() const;
    [[nodiscard]] std::string BuildTurnPosture(const std::string& policyInput, const std::vector<conversationMessage>& promptContext,
        const aiProfile& profile, bool llmAvailable, const TurnPolicy& turnPolicy) const;
    // Runs one bounded deliberation when the router already judged this turn hard,
    // publishes the questions so they are visible in chat, and returns what she worked
    // out. Returns an empty result whenever the gate stays shut, and a failed pass is
    // never fatal to the turn: she answers as she would have without it.
    // What further rounds established, if any ran.
    struct InvestigationSummary
    {
        bool ran = false;
        // Appended to the turn's posture, exactly as the single inquiry's block is.
        std::string promptBlock;
        std::size_t rounds = 0;
        std::size_t observations = 0;
        agents::InvestigationOutcome outcome = agents::InvestigationOutcome::Running;
        std::string reason;
        double elapsedMilliseconds = 0.0;
    };

    // Continues a completed self-inquiry into further rounds.
    //
    // Round one is the existing SelfInquiryAgent pass, unchanged. This seeds an
    // investigation from the questions it produced and lets later rounds choose their
    // questions from what earlier rounds actually found -- which is the whole of what the
    // single pass could not do.
    [[nodiscard]] InvestigationSummary RunInvestigation(const agents::SelfInquiryResult& seed, const std::string& policyInput,
        const std::string& basePosture, std::uint64_t turnId, std::stop_token stopToken, const agents::CheckExecutor& executor,
        const std::function<bool()>& admission, std::uint64_t audienceRevision);

    [[nodiscard]] agents::SelfInquiryResult RunSelfInquiry(const std::string& policyInput,
        const std::vector<conversationMessage>& promptContext, const std::string& basePosture,
        const intelligence::IntelligenceDecision& routing, bool modelAvailable, std::uint64_t turnId, std::stop_token stopToken,
        std::uint64_t audienceRevision);
    [[nodiscard]] agents::ResponseFilterContext BuildResponseFilterContext(const std::string& policyInput,
        const std::vector<conversationMessage>& promptContext) const;
    [[nodiscard]] std::string BuildArithmeticGrounding(const std::string& input, const std::vector<conversationMessage>& promptContext,
        std::stop_token stopToken, const std::function<bool()>& admission = {}) const;
    SessionResult Generate(const std::string& policyInput, const std::vector<conversationMessage>& promptContext, const aiProfile& profile,
        bool llmAvailable, bool shouldSpeak, bool evaluateMemory, bool proactive, const std::string& proactiveInstruction,
        const std::string& precomputedInternetGrounding, std::stop_token stopToken, const TurnPolicy& turnPolicy);
    void PublishComponent(const std::string& component, const std::string& phase, const std::string& message, double elapsedMilliseconds,
        int queueDepth, std::uint64_t turnId, std::uint64_t audienceRevision = 0) const;
    void PublishInternetActivity(const std::string& phase, const std::string& query, const std::string& provider, const std::string& detail,
        double elapsedMilliseconds, int sourceCount, std::uint64_t turnId, std::uint64_t audienceRevision = 0) const;

    messageRouter& router;
    conversationContext& context;
    agents::TurnCoordinator& coordinator;
    speech::SpeechService& speech;
    AffectController& affect;
    // Canonical state. AffectController above is retained for comparison only.
    emotion::EmotionRuntime& emotions;
    RuntimeEventBus& events;
    logger& log;
    StateHandler setState;
    std::function<std::function<bool()>()> privateAdmissionFactory;
    InvestigationExecutorFactory investigationExecutorFactory;
    ResponseLatency responseLatency;
    RuntimeEventBus::SubscriptionId latencySubscription = 0;
    AffectHandler publishAffect;
    InternetSettingsProvider internetSettings;
    DesktopSettingsProvider desktopSettings;
    InternetLookupHandler internetLookup;
    ResponseFilterSettingsProvider filterSettingsProvider;
    ScreenContextProvider screenContextProvider;
    RelationshipProvider relationshipProvider;
    DevelopmentProvider developmentProvider;
    PreferenceProvider preferenceProvider;
    StimulusObserver stimulusObserver;
    ScreenCaptureRequest screenCaptureRequest;
    SelfInquirySettingsProvider selfInquirySettingsProvider;
    ConversationRecallHandler conversationRecall;
    AutonomyContextProvider autonomyContextProvider;
    SongListProvider songListProvider;
    agents::ConversationQualityMonitor qualityMonitor;
    intelligence::HumanizationController humanization;
    intelligence::IntelligenceRouter intelligenceRouter;
    intelligence::ReflexRouter reflexRouter;
    // Deliberation is part of running a turn, so it lives here beside routing rather than
    // becoming a second owner of conversation. The policy holds the cooldown; the agent
    // holds the one bounded model call.
    agents::SelfInquiryPolicy selfInquiryPolicy;
    agents::SelfInquiryAgent selfInquiryAgent;
    // Task-scoped: rebuilt per turn, so a round from a superseded question can never be
    // read back under a later one.
    agents::Investigation activeInvestigation;
    std::string previousReflexResponse;
    std::string previousReflexInput;
    std::size_t repeatedReflexCalls = 0;
    // The tier that produced the last answer this conversation actually delivered, so a
    // short follow-up can inherit its effort. Set at the one point a reply enters
    // conversation history, which is why a cancelled, failed, or empty generation never
    // reaches it. Transient and deliberately not persisted: routing effort is not
    // identity, and a restarted session starts with no tier rather than a guessed one.
    std::optional<intelligence::IntelligenceTier> previousDeliveredTier;
    // Recorded beside the tier, at the same one point, and for the same reason: the
    // next turn's routing needs to know whether the answer it is following up on was
    // one the runtime itself had cause to doubt. Set from what the runtime observed --
    // a generation that failed, a reply the deterministic filter had to replace, a
    // reply the quality monitor found ungrounded -- and never from asking the model how
    // sure it was. Transient, like the tier.
    bool previousTurnWasUnreliable = false;
    std::uint64_t turnCounter = 0;
    std::uint64_t utteranceCounter = 0;
};

} // namespace revia::runtime
