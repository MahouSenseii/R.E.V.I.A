#pragma once

#include "Actions/actionRuntime.h"
#include "Agents/curiosityAgent.h"
#include "Agents/turnCoordinator.h"
#include "Agents/inputArbiter.h"
#include "Core/commandManager.h"
#include "Core/configManager.h"
#include "Core/conversationContext.h"
#include "Content/workingDocument.h"
#include "Core/preferenceStore.h"
#include "Core/logger.h"
#include "Core/messageRouter.h"
#include "Evaluation/conversationEvaluation.h"
#include "Memory/conversationArchive.h"
#include "Goals/goalRunner.h"
#include "Goals/goalSandbox.h"
#include "Goals/goalStore.h"
#include "LLM/LLamaCPP/llamaCppServerProcess.h"
#include "Initiative/initiativeController.h"
#include "Initiative/conversationStarter.h"
#include "Initiative/curiosityJournal.h"
#include "Learning/learningReview.h"
#include "Perception/activityHistory.h"
#include "Perception/windowEventMonitor.h"
#include "Presence/presenceRuntime.h"
#include "Runtime/affectController.h"
#include "Runtime/conversationRuntime.h"
#include "Runtime/runtimeEvents.h"
#include "Runtime/sessionResult.h"
#include "Resources/resourceMonitor.h"
#include "Resources/resourcePlanner.h"
#include "Speech/speechService.h"
#include "Speech/speechRecognitionService.h"
#include "Vision/screenCaptureService.h"
#include "Vision/visionActionParser.h"
#include "Windows/visionUiaResolver.h"
#include "Visual/imageGenerator.h"
#include "Visual/svgCanvas.h"
#include "Windows/applicationControlDiscovery.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace revia::runtime
{

struct CapabilityUpdateResult
{
    bool succeeded = false;
    std::string message;
};

// Small read-only view for desktop controls. It contains comfort/diagnostic preferences
// only; capability authority continues to live behind CapabilityEditor.
struct UserPreferenceSnapshot
{
    bool speechEnabled = true;
    bool bargeInEnabled = true;
    bool handsFreeEnabled = false;
    bool avatarBridgeEnabled = true;
    bool externalAdaptersEnabled = false;
    bool initiativeEnabled = false;
    bool curiosityEnabled = false;
    bool spontaneousSpeechEnabled = false;
    bool speakWhenUserAway = false;
    bool aiResponseReviewEnabled = true;
    int initiativeMaxPerHour = 0;
    int resourceSampleSeconds = 0;
};

class ReviaSession
{
public:
    using ConfirmationHandler = std::function<bool(
        const actions::ActionRequest&,
        const actions::PolicyDecision&)>;

    ReviaSession();
    ~ReviaSession();

    ReviaSession(const ReviaSession&) = delete;
    ReviaSession& operator=(const ReviaSession&) = delete;

    bool Start();
    SessionResult Submit(
        const std::string& input,
        agents::InputSource source = agents::InputSource::Typed);

    // Voice arrives as a stream, not as questions. Offering it here lets several bursts
    // merge into one turn and lets room noise be dropped before it becomes a reply. The
    // answer arrives as an AssistantMessage event rather than a return value, because the
    // turn starts when the merge window closes rather than when the caller asks.
    agents::InputVerdict OfferInput(const std::string& text, agents::InputSource source);
    void PollBackgroundEvents();
    void RequestStop();
    void Stop();

    void SetConfirmationHandler(ConfirmationHandler handler);
    RuntimeEventBus& Events();
    RuntimeState State() const;
    bool IsStarted() const;
    bool IsBusy() const;
    bool IsSpeechEnabled() const;
    void SetSpeechEnabled(bool enabled);
    void SetBargeInEnabled(bool enabled);
    [[nodiscard]] bool IsBargeInEnabled() const;
    void SetHandsFreeEnabled(bool enabled);
    [[nodiscard]] bool IsHandsFreeEnabled() const;
    [[nodiscard]] presence::PresenceSnapshot Presence() const;
    bool BeginListening();
    bool EndListening();
    [[nodiscard]] bool IsVisionAvailable() const;
    SessionResult AnalyzeScreen(const std::string& prompt);
    // The model may locate a target, but it cannot click it. A successful request must
    // resolve to an exact UIA runtime id and then pass through ordinary action policy,
    // confirmation, dispatch, and audit.
    SessionResult ActOnScreen(const std::string& instruction);

    [[nodiscard]] actions::CapabilitySettings Capabilities() const;
    [[nodiscard]] actions::windows::ApplicationControlInventory
        DiscoverForegroundApplicationControls() const;
    CapabilityUpdateResult AddApprovedApplication(const std::string& executable);
    CapabilityUpdateResult RemoveApprovedApplication(const std::string& executable);
    CapabilityUpdateResult AddApprovedControl(
        const std::string& executable,
        const std::string& control);
    CapabilityUpdateResult RemoveApprovedControl(
        const std::string& executable,
        const std::string& control);
    CapabilityUpdateResult SetInternetAccess(bool enabled, bool automaticLookup);
    CapabilityUpdateResult SetInternetBrowser(bool visibleBrowser, bool autonomousResearch);

    // Stage 4. The runner itself adds no authority: every step goes through the same
    // dispatcher, policy, and audit path as an interactive action, and has to prove it
    // happened before the goal advances.
    goals::Goal RunGoal(goals::Goal goal);
    goals::Goal ResumeGoal(const std::string& goalId);
    [[nodiscard]] std::vector<goals::Goal> RecentGoals(std::size_t maxGoals = 25) const;
    [[nodiscard]] std::vector<goals::Goal> ResumableGoals() const;

    // Live usage measured against the immutable plan. Reading only: the planner decides
    // placement once at startup and is never re-run from a sample, because moving a
    // worker because a number moved turns a reproducible plan into a feedback loop.
    [[nodiscard]] resources::UsageSnapshot ResourceUsage() const;
    [[nodiscard]] std::string ResourceUsageStatus() const;

    // Durable conversation history. Separate from longTermMemory, which keeps curated
    // facts: this keeps what was actually said, bounded and forgettable.
    [[nodiscard]] std::string ConversationHistoryStatus() const;
    [[nodiscard]] std::vector<memory::ArchivedTurn> SearchConversations(
        const std::string& query, std::size_t maxTurns = 12) const;
    [[nodiscard]] std::vector<memory::ArchivedSession> RecentConversations(
        std::size_t maxSessions = 20) const;
    std::size_t ForgetConversations();

    // Durable non-authority settings. The store cannot reach a capability, so nothing
    // here can widen what Revia is permitted to do.
    core::PreferenceResult SetPreference(const std::string& name, const std::string& value);
    [[nodiscard]] std::string DescribePreferences() const;
    [[nodiscard]] std::string VoiceDevicePreference() const;
    [[nodiscard]] UserPreferenceSnapshot UserPreferences() const;

    // Draws an explanatory diagram or interface mockup. The model produces SVG, the
    // sanitizer refuses anything that would run or fetch, and the result is a file.
    SessionResult DrawDiagram(const std::string& request);
    [[nodiscard]] std::vector<visual::Diagram> RecentDiagrams(
        std::size_t maxDiagrams = 20) const;
    // Puts an existing picture on the canvas. Read-only and bounded by the same approved
    // roots that govern reading a file, because displaying one is reading one.
    SessionResult ShowPicture(const std::string& path);
    // A generated picture, not a diagram. Separate capability because the two cannot
    // substitute for each other: a language model emitting SVG draws boxes and arrows and
    // cannot draw a scene, and an image model draws a scene and cannot lay out a panel.
    SessionResult GenerateImage(const std::string& prompt);

    // The working document. Generation is wholesale and says so; an edit reaches exactly
    // one block, because ReplaceBlock is the only mutation the edit path can express.
    SessionResult ComposeDocument(const std::string& request);
    SessionResult ReviseDocumentBlock(
        const std::string& reference,
        const std::string& instruction);
    [[nodiscard]] const content::WorkingDocument& Document() const;

    // Stage 6 Tier 0. Window and focus events only; no capture and no model.
    [[nodiscard]] bool IsPerceptionEnabled() const;
    [[nodiscard]] bool IsPerceptionPaused() const;
    void SetPerceptionPaused(bool paused);
    [[nodiscard]] perception::PerceptionCounters PerceptionCounters() const;
    [[nodiscard]] std::string PerceptionStatus() const;
    // Stage 6's exit criterion: describe what the last stretch of time was spent on from
    // Tier 0 evidence alone, with no model and no capture.
    [[nodiscard]] std::string RecentActivity(std::chrono::minutes window) const;
    void ForgetActivity();

    // Where Revia is talking. Composing into another application is text; only the local
    // channel is read aloud, unless that executable is explicitly opted in.
    void SetOutputChannel(outputChannel channel, const std::string& applicationName = {});
    [[nodiscard]] bool ShouldSpeakOnCurrentChannel() const;
    [[nodiscard]] std::string OutputChannelStatus() const;

    // Stage 7. Revia offers, the user disposes. A proposal executes nothing on its own.
    [[nodiscard]] std::string InitiativeStatus() const;
    [[nodiscard]] std::vector<initiative::Proposal> PendingProposals() const;
    SessionResult AcceptProposal(const std::string& proposalId);

    // Runs the conversation contract corpus against the active local model.
    //
    // Deterministic tests prove the assembly around a reply is correct; they cannot prove
    // that this model, at this temperature, still honours the contract. This does, at the
    // cost of real inference time, and it is honest about its ceiling: it detects
    // known-bad replies and cannot certify a good one.
    [[nodiscard]] evaluation::EvaluationReport RunConversationEvaluation(
        const std::vector<evaluation::EvaluationCase>& cases,
        std::stop_token stopToken = {});
    [[nodiscard]] evaluation::EvaluationReport LastConversationEvaluation() const;

    // Stage 4's reviewed learning. Lessons are drawn from recorded outcomes and offered;
    // approving one writes an ordinary memory entry. Nothing here changes a capability, a
    // budget, or a policy, and nothing is stored without being approved.
    [[nodiscard]] std::vector<learning::Lesson> DrawLessons() const;
    bool ApproveLesson(const std::string& lessonId, std::string& outSummary);
    void DismissProposal(const std::string& proposalId);
    std::string DisplayName() const;
    std::string Greeting() const;
    speech::VoiceStudioSnapshot VoiceStudio() const;
    speech::VoiceOperationResult CreateVoicePreset(
        const std::string& name,
        const std::string& description,
        const std::string& referenceText,
        const std::string& language);
    speech::VoiceOperationResult PreviewVoice(
        const std::string& presetId,
        const std::string& text);
    speech::VoiceOperationResult AssignVoice(
        const std::string& profileId,
        const std::string& presetId);

private:
    bool EnsureLLMAvailable(std::stop_token stopToken);
    bool EnsureEmbeddingAvailable(std::stop_token stopToken);
    bool TryHandleActionInput(const std::string& input, SessionResult& result);
    SessionResult ExecuteAction(actions::ActionRequest request);
    static std::string FormatActionOutcome(const actions::ActionOutcome& outcome);
    // Submit already holds operationMutex when a /goals command arrives, and that mutex is
    // not recursive, so the command path uses these and the public entry points lock.
    goals::Goal RunGoalUnlocked(goals::Goal goal);
    goals::Goal ResumeGoalUnlocked(const std::string& goalId);
    goals::Goal FinishGoalRun(goals::Goal finished, std::chrono::steady_clock::time_point startedAt);
    void PublishGoalProgress(const goals::GoalProgress& progress) const;
    static std::string FormatGoalSummary(const goals::Goal& goal);
    static std::string FormatGoalList(const std::vector<goals::Goal>& goalList);
    static std::string FormatGoalPlan(const goals::Goal& goal);
    // Runs the plan against a throwaway copy first, so the plan is approved on observed
    // evidence rather than on how reasonable its text looked.
    goals::Goal RehearseGoal(const goals::Goal& goal, std::string& outSummary);
    // Narrowed from the configured policy, never read from the plan. A goal that chose
    // its own scope could widen its own authority, which is the one thing the scoped
    // execution path exists to prevent.
    [[nodiscard]] actions::CapabilitySettings DeriveGoalScope() const;
    bool TryHandleGoalInput(const std::string& input, SessionResult& result);
    // Every archived turn goes through here, so the sensitive-content refusal and the
    // enabled check live in one place rather than at each call site.
    void ArchiveTurn(const std::string& role, const std::string& content);
    // Replays the tail of the previous session into context, so a restart continues a
    // conversation rather than starting one that has forgotten yesterday.
    void RestoreConversationContext();
    // Submit already holds operationMutex when /eval arrives; the public entry point locks.
    evaluation::EvaluationReport RunConversationEvaluationUnlocked(
        const std::vector<evaluation::EvaluationCase>& cases,
        std::stop_token stopToken);
    // The checked-in corpus unless RuntimeData supplies one, so cases can be added
    // without a rebuild and an edited corpus cannot be silently restored by one.
    [[nodiscard]] std::vector<evaluation::EvaluationCase> LoadEvaluationCorpus(
        std::string& outSource);
    void StartVoiceWarmup();
    void StopVoiceWarmup();
    // Shared by the immediate typed path and the merged voice path. Callers hold
    // operationMutex; the arbiter has already decided what the turn's text is.
    SessionResult RunTurnLocked(const std::string& acceptedInput);
    // Runs merged voice turns once their window closes. Its own thread rather than the
    // shell's timer, so listening does not depend on a debug window being open.
    void StartInputDrain();
    void StopInputDrain();
    // Its own thread, not the shell's poll timer. A companion that only considers speaking
    // while a debug window happens to be open is not a companion.
    void StartInitiativeLoop();
    void StopInitiativeLoop();
    void SignalInitiative(const std::string& reason);
    void StartCuriosityLoop();
    void StopCuriosityLoop();
    void SignalCuriosity(const std::string& reason);
    void StartExternalAdapterLoop();
    void StopExternalAdapterLoop();
    void QueueExternalAdapterEvent(const presence::ExternalAdapterEvent& event);
    std::stop_token BeginOperation();
    // The token for the operation already in flight. BeginOperation replaces the stop
    // source, so a nested run must not call it: doing so would discard a stop the user
    // requested while the outer operation was still dispatching.
    [[nodiscard]] std::stop_token CurrentOperationToken() const;
    void SetState(RuntimeState newState, const std::string& activity = "");
    void PublishAffect(const AffectSnapshot& affect);
    void Publish(RuntimeEventKind kind, const std::string& message, std::uint64_t turnId = 0) const;
    void PublishComponent(
        const std::string& component,
        const std::string& phase,
        const std::string& message,
        double elapsedMilliseconds = -1.0,
        int queueDepth = 0,
        std::uint64_t turnId = 0,
        const std::string& resource = {}) const;
    void PublishResourcePlan() const;
    void PublishResourceUsage(const resources::UsageSnapshot& snapshot) const;
    void StartResourceMonitor();

    RuntimeEventBus eventBus;
    logger appLogger;
    messageRouter router;
    configManager config;
    commandManager commands;
    conversationContext context;
    appSettings settings;
    aiProfile profile;
    resources::ResourcePlan resourcePlan;
    resources::ResourceMonitor resourceMonitor;
    actions::ActionRuntime actionRuntime;
    // Declared after actionRuntime: GoalRunner holds references to both of these.
    goals::GoalStore goalStore;
    goals::GoalRunner goalRunner;
    AffectController affectController;
    speech::SpeechService speechService;
    speech::SpeechRecognitionService speechRecognitionService;
    presence::PresenceRuntime presenceRuntime;
    perception::WindowEventMonitor windowEventMonitor;
    perception::ActivityHistory activityHistory;
    initiative::InitiativeController initiativeController;
    initiative::ConversationStarter conversationStarter;
    initiative::CuriosityJournal curiosityJournal;
    agents::CuriosityAgent curiosityAgent;
    vision::ScreenCaptureService screenCaptureService;
    vision::VisionActionParser visionActionParser;
    actions::windows::VisionUiaResolver visionUiaResolver;
    actions::windows::ApplicationControlDiscovery applicationControlDiscovery;
    llamaCppServerProcess llamaServerProcess;
    llamaCppServerProcess embeddingServerProcess;
    agents::TurnCoordinator turnCoordinator;
    ConversationRuntime conversationRuntime;
    agents::InputArbiter inputArbiter;

    evaluation::EvaluationReport lastEvaluation;
    memory::ConversationArchive conversationArchive;
    core::PreferenceStore preferenceStore;
    visual::DiagramStore diagramStore;
    content::WorkingDocument workingDocument;
    visual::ImageGenerator imageGenerator;
    std::string conversationSessionId;

    mutable std::mutex operationMutex;
    mutable std::mutex cancellationMutex;
    mutable std::mutex confirmationMutex;
    mutable std::mutex voiceStudioMutex;
    mutable std::mutex channelMutex;
    mutable std::mutex initiativeSignalMutex;
    mutable std::mutex curiositySignalMutex;
    mutable std::mutex externalAdapterMutex;
    std::condition_variable_any externalAdapterCondition;
    std::deque<presence::ExternalAdapterEvent> externalAdapterQueue;
    std::condition_variable_any initiativeCondition;
    std::condition_variable_any curiosityCondition;
    std::uint64_t initiativeSignalVersion = 0;
    std::string initiativeSignalReason;
    std::uint64_t curiositySignalVersion = 0;
    std::string curiositySignalReason;
    std::stop_source curiosityAttemptStopSource;
    outputChannel outputTarget = outputChannel::LocalVoice;
    std::string outputApplication;
    std::stop_source activeStopSource;
    ConfirmationHandler confirmationHandler;
    // Loads the assigned Qwen3-TTS voice after startup has already reported ready.
    std::jthread voiceWarmupWorker;
    std::atomic<bool> voiceWarmupFinished = true;
    std::jthread initiativeWorker;
    std::jthread curiosityWorker;
    std::jthread inputDrainWorker;
    std::jthread externalAdapterWorker;
    RuntimeEventBus::SubscriptionId presenceSubscriptionId = 0;
    std::atomic<RuntimeState> state = RuntimeState::Offline;
    std::atomic<bool> started = false;
    std::atomic<bool> busy = false;
    std::atomic<bool> llmAvailable = false;
    std::atomic<std::uint64_t> userInteractionGeneration = 0;
    std::atomic<std::uint64_t> curiosityRunCounter = 0;
    std::atomic<std::int64_t> lastUserInteractionSteadyMs = 0;
    // Live response-filter controls are read by the conversation worker while the UI may
    // change the preference. Keep that handoff atomic instead of reading appSettings
    // concurrently; token/length ceilings are immutable after startup.
    std::atomic<bool> responseAiReviewEnabled = true;
    int responseAiMaxReviewTokens = 192;
    int responseMaxReplyCharacters = 12000;
};

} // namespace revia::runtime
