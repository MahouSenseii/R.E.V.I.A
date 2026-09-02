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
#include "Memory/conversationRecall.h"
#include "Goals/goalRunner.h"
#include "Goals/goalSandbox.h"
#include "Goals/goalStore.h"
#include "Autonomy/activityScheduler.h"
#include "Emotion/emotionRuntime.h"
#include "Identity/developmentEngine.h"
#include "Identity/preferenceEvidence.h"
#include "Identity/relationshipRegistry.h"
#include "LLM/LLamaCPP/llamaCppServerProcess.h"
#include "Initiative/initiativeController.h"
#include "Initiative/conversationStarter.h"
#include "Initiative/curiosityJournal.h"
#include "Learning/learningReview.h"
#include "Learning/selfAssessment.h"
#include "Perception/activityHistory.h"
#include "Perception/windowEventMonitor.h"
#include "Presence/presenceRuntime.h"
#include "Runtime/affectController.h"
#include "Runtime/conversationRuntime.h"
#include "Runtime/runtimeEvents.h"
#include "Runtime/sessionResult.h"
#include "Resources/loadGovernor.h"
#include "Resources/resourceMonitor.h"
#include "Resources/resourcePlanner.h"
#include "Speech/speechService.h"
#include "Speech/speechRecognitionService.h"
#include "Vision/cameraCaptureService.h"
#include "Vision/screenCaptureService.h"
#include "Vision/visionActionParser.h"
#include "Windows/visionUiaResolver.h"
#include "Visual/imageGenerator.h"
#include "Visual/svgCanvas.h"
#include "Windows/applicationControlDiscovery.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace revia::runtime
{

struct CapabilityUpdateResult
{
    bool succeeded = false;
    std::string message;
};

// One profile as the desktop profile editor sees it: what it makes Revia say, and which
// created voice speaks it. Voice assignment lives beside the profile because that is what
// it is -- a property of who is talking, not of the speech engine.
struct ProfileSummary
{
    std::string id;
    std::string displayName;
    std::string description;
    std::string systemPrompt;
    bool memoryEnabled = true;
    bool hasTemperatureOverride = false;
    float temperature = 0.7f;
    bool hasMaxTokensOverride = false;
    int maxTokens = 512;
    // Empty means this profile falls back to the Windows voice.
    std::string voicePresetId;
    std::string voicePresetName;
};

struct ProfileStudioSnapshot
{
    std::vector<ProfileSummary> profiles;
    // The profile Revia is running right now, which is the only honest answer to "which
    // profile is in use". It is read from the loaded profile, not from settings.
    std::string activeProfileId;
    std::string activeDisplayName;
    // The created voices a profile can be assigned, so the editor does not need a second
    // trip through the voice studio to render its picker.
    std::vector<speech::VoicePreset> voices;
};

struct ProfileOperationResult
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

    // Camera. Off unless the capability file says otherwise, and rate limited even then.
    //
    // Listing devices is separate from using one so a settings screen can show what is
    // attached without lighting a lens, and CaptureCameraFrame refuses rather than
    // silently succeeding when the capability is absent -- a camera that quietly works
    // when the user believes it is off is the worst possible failure here.
    [[nodiscard]] bool IsCameraAvailable() const;
    // Display topology, read without capturing anything, so a settings screen can show
    // what is attached without needing screen-capture permission first.
    // Captures and describes the screen right now, for a turn that explicitly asked
    // about it.
    //
    // Continuous awareness is off by default and deliberately so, which meant "what is
    // on my screen?" had no way to actually look: the cached observation it reads was
    // only ever populated by the awareness loop. Being asked is the consent here, the
    // same way it is for the camera -- answering a question about the screen is not the
    // same as watching it.
    [[nodiscard]] std::string CaptureScreenContextNow();
    [[nodiscard]] std::vector<vision::MonitorDescriptor> Monitors() const;
    [[nodiscard]] std::vector<vision::CameraDescriptor> Cameras() const;
    // autonomous is true when Revia chose to look rather than being asked. It requires
    // the separate autonomousCapture authority on top of camera access.
    vision::CameraFrame CaptureCameraFrame(bool autonomous = false);
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
    CapabilityUpdateResult SetCameraAccess(bool enabled, bool autonomousCapture);

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
    // What the machine can currently afford. Advisory: it changes what optional work is
    // attempted, never where a model lives.
    [[nodiscard]] resources::LoadAdjustment CurrentLoad() const;

    // Curated long-term memory, read-only. What Revia actually kept, so a user can see
    // it rather than infer it from what she happens to bring up. Reading cannot write:
    // memory is added through the reviewed memory path, never from a viewer.
    // Who Revia is talking to, and how she stands with them. Read-only from outside:
    // relationships move only through recorded evidence, never by assignment.
    [[nodiscard]] std::vector<identity::RelationshipState> Relationships() const;
    [[nodiscard]] identity::RelationshipState CurrentRelationship() const;
    [[nodiscard]] emotion::EmotionVector CurrentEmotion() const;
    [[nodiscard]] emotion::MoodState CurrentMood() const;
    [[nodiscard]] identity::DevelopmentState CurrentDevelopment() const;
    // What she currently likes and dislikes. Read-only: opinions move from evidence, not
    // by being set from outside.
    [[nodiscard]] std::vector<identity::Preference> CurrentPreferences() const;
    // What is currently pulling at her, and what the scheduler last decided about it.
    // Read-only: drives move from stimuli and time, never by being set.
    [[nodiscard]] autonomy::DriveState Drives() const;
    [[nodiscard]] autonomy::ActivityDecision LastAutonomyDecision() const;
    [[nodiscard]] std::optional<autonomy::Activity> CurrentActivity() const;
    // Every applied personality change, with the evidence behind it.
    [[nodiscard]] std::vector<identity::DevelopmentChange> DevelopmentHistory() const;

    [[nodiscard]] std::vector<memoryEntry> Memories() const;
    [[nodiscard]] std::vector<memoryEntry> SearchMemories(
        const std::string& query, std::size_t maxEntries = 50) const;
    [[nodiscard]] std::string MemoryStatus() const;

    // Durable conversation history. Separate from longTermMemory, which keeps curated
    // facts: this keeps what was actually said, bounded and forgettable.
    [[nodiscard]] std::string ConversationHistoryStatus() const;
    [[nodiscard]] std::vector<memory::ArchivedTurn> SearchConversations(
        const std::string& query, std::size_t maxTurns = 12) const;
    // Everything said in a window of epoch seconds, oldest first. Backs /history with a
    // date or a phrase like "yesterday" instead of words to match.
    [[nodiscard]] std::vector<memory::ArchivedTurn> ConversationsInRange(
        std::int64_t startEpoch,
        std::int64_t endEpoch,
        std::size_t maxTurns = 40) const;
    // Answers one typed recall request from the conversational path and renders the
    // bounded block that grounds the reply. Returns empty when archiving is off, when
    // nothing matches, or when the only match was the question being asked.
    [[nodiscard]] std::string RecallConversation(
        const memory::RecallRequest& request,
        const std::string& currentInput) const;
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

    // Stage 6. Tier 0 window/focus events optionally wake a bounded local visual summary;
    // neither observation path grants action authority.
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

    // Profiles. Creating and editing one is a file write; making one current swaps the
    // system prompt, sampling, and assigned voice in place. Neither can reach a
    // capability: a profile decides who Revia is, never what she is permitted to do.
    [[nodiscard]] ProfileStudioSnapshot ProfileStudio() const;
    ProfileOperationResult SaveProfile(const ProfileSummary& definition);
    ProfileOperationResult ActivateProfile(const std::string& profileId);

private:
    // The profile owns where she starts: trait baseline and declared opinions. What
    // experience has earned -- trait drift, and any preference she already holds --
    // survives, or editing a profile would quietly delete her development.
    //
    // Applied at startup and whenever the active profile changes, so an edit takes effect
    // without a restart. Private: re-seating who she started as is a consequence of
    // loading a profile, not something a caller may ask for on its own.
    void ApplyProfilePersonality();
    bool EnsureLLMAvailable(std::stop_token stopToken);
    bool EnsureFastBrainAvailable(std::stop_token stopToken);
    bool EnsureExpertBrainAvailable(std::stop_token stopToken);
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
    // Reads observable signals out of a finished turn and applies them as bounded
    // relationship evidence. Deterministic: no model is consulted about how Revia should
    // feel toward someone, because a model that could set those numbers would let anyone
    // talk their way into being trusted.
    // Applies bounded preference evidence and reports only the opinions that actually
    // moved. Separate from RecordRelationshipEvidence because what she thinks of a
    // person and what she thinks of a subject are different things that must not be
    // able to overwrite one another (design §8, §10).
    void RecordPreferenceEvidence(
        const std::vector<identity::PreferenceObservation>& observations);
    void RecordRelationshipEvidence(
        const std::string& entityId,
        const std::string& userInput,
        const std::string& reply,
        bool succeeded);
    void PersistIdentity();
    // Records what a finished turn says about who she is becoming. Bounded and slow:
    // several consistent observations are needed before anything moves at all.
    void RecordDevelopmentEvidence(const identity::TurnObservation& observation);
    // Moves drives from a confirmed event, alongside the emotional appraisal of it, so
    // wanting and feeling never disagree about what happened.
    void ObserveDrives(const emotion::Stimulus& stimulus);
    // Asks whether there is any reason to act. Called from the initiative loop rather
    // than from a timer of its own: a timer may permit an activity, never motivate one.
    void ConsiderAutonomousActivity(const std::string& triggerReason);
    [[nodiscard]] autonomy::AutonomyEvidence GatherAutonomyEvidence() const;
    [[nodiscard]] autonomy::AutonomyCost GatherAutonomyCost() const;
    // Interrupts whatever she chose to do because the user needs attention. Interrupted
    // is not cancelled: what was cut off stays resumable.
    void PreemptAutonomousActivity(const std::string& because);
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
    void StartScreenAwareness();
    void StopScreenAwareness();
    void SignalScreenAwareness(const std::string& reason);
    void CancelScreenAwarenessAttempt();
    [[nodiscard]] std::string CurrentScreenContext() const;
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
    // Primary emotion path. AffectController remains as the deterministic fallback and
    // baseline; this is what reaches the prompt, the badge, and speech.
    emotion::EmotionRuntime emotionRuntime;
    identity::DevelopmentEngine developmentEngine;
    autonomy::DriveController driveController;
    autonomy::ActivityScheduler activityScheduler;
    mutable std::mutex autonomyMutex;
    autonomy::DriveState drives;
    autonomy::ActivityDecision lastAutonomyDecision;
    // What was last written to the log, so a repeated identical decision is published as
    // an event but not logged again. Logging every idle evaluation would bury the log;
    // logging none of them makes a silent Revia impossible to diagnose.
    std::string lastLoggedAutonomy;
    mutable std::mutex loadMutex;
    resources::LoadAdjustment currentLoad;
    // Hysteresis lives here rather than in the governor, which is pure. Without it a
    // reading hovering on a threshold flips the machine between states every sample.
    resources::LoadState lastPublishedLoad = resources::LoadState::Normal;
    // A new state has to hold for several consecutive samples before it is adopted.
    // VRAM readings swing hard while models load and free memory -- 111%, then 11%, then
    // 88% within seconds -- and acting on each swing made what Revia would attempt
    // change from one moment to the next for no reason a person could see.
    resources::LoadState candidateLoad = resources::LoadState::Normal;
    int candidateLoadSamples = 0;
    static constexpr int loadSamplesBeforeAdopting = 3;
    std::optional<autonomy::Activity> runningActivity;
    // Rolling counters the scheduler charges against. Kept here rather than in the
    // scheduler so it stays a pure function of its inputs.
    std::deque<std::chrono::steady_clock::time_point> recentActivities;
    std::chrono::steady_clock::time_point lastActivityAt{};
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
    vision::CameraCaptureService cameraCaptureService;
    mutable std::mutex cameraMutex;
    std::chrono::steady_clock::time_point lastCameraCaptureAt{};
    std::deque<std::chrono::steady_clock::time_point> recentCameraCaptures;
    vision::VisionActionParser visionActionParser;
    actions::windows::VisionUiaResolver visionUiaResolver;
    actions::windows::ApplicationControlDiscovery applicationControlDiscovery;
    llamaCppServerProcess llamaServerProcess;
    llamaCppServerProcess fastServerProcess;
    llamaCppServerProcess expertServerProcess;
    llamaCppServerProcess embeddingServerProcess;
    llmSettings fastLlmSettings;
    llmSettings expertLlmSettings;
    bool fastBrainConfigured = false;
    bool expertBrainConfigured = false;
    agents::TurnCoordinator turnCoordinator;
    ConversationRuntime conversationRuntime;
    agents::InputArbiter inputArbiter;

    evaluation::EvaluationReport lastEvaluation;
    memory::ConversationArchive conversationArchive;
    core::PreferenceStore preferenceStore;
    identity::RelationshipRegistry relationships;
    // The entity whose turn is being handled. Set before a turn runs and read when the
    // state packet is assembled, so relationship state follows whoever is speaking
    // rather than being global.
    mutable std::mutex speakerMutex;
    std::string currentSpeakerId = identity::LocalUserEntityId();
    learning::SelfAssessmentEngine selfAssessment;
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
    mutable std::mutex screenAwarenessMutex;
    std::condition_variable_any externalAdapterCondition;
    std::condition_variable_any screenAwarenessCondition;
    std::deque<presence::ExternalAdapterEvent> externalAdapterQueue;
    // Public history is deliberately channel-scoped and memory-only. It never enters
    // the local user's conversationContext or durable conversation archive.
    std::unordered_map<std::string, std::deque<conversationMessage>>
        publicConversationContexts;
    std::condition_variable_any initiativeCondition;
    std::condition_variable_any curiosityCondition;
    std::uint64_t initiativeSignalVersion = 0;
    std::string initiativeSignalReason;
    std::uint64_t curiositySignalVersion = 0;
    std::string curiositySignalReason;
    std::stop_source curiosityAttemptStopSource;
    std::stop_source screenAwarenessAttemptStopSource;
    std::uint64_t screenAwarenessSignalVersion = 0;
    std::string screenAwarenessSignalReason;
    std::string latestScreenContext;
    std::chrono::steady_clock::time_point latestScreenContextAt{};
    outputChannel outputTarget = outputChannel::LocalVoice;
    std::string outputApplication;
    std::stop_source activeStopSource;
    ConfirmationHandler confirmationHandler;
    // Loads the assigned Qwen3-TTS voice after startup has already reported ready.
    std::jthread voiceWarmupWorker;
    std::atomic<bool> voiceWarmupFinished = true;
    std::jthread screenAwarenessWorker;
    std::jthread initiativeWorker;
    std::jthread curiosityWorker;
    std::jthread inputDrainWorker;
    std::jthread externalAdapterWorker;
    RuntimeEventBus::SubscriptionId presenceSubscriptionId = 0;
    RuntimeEventBus::SubscriptionId selfAssessmentSubscriptionId = 0;
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
