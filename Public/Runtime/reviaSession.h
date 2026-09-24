#pragma once

#include "Actions/actionRuntime.h"
#include "Agents/curiosityAgent.h"
#include "Agents/turnCoordinator.h"
#include "Agents/inputArbiter.h"
#include "Core/commandManager.h"
#include "Core/configManager.h"
#include "Core/conversationContext.h"
#include "Content/workingDocument.h"
#include "Runtime/documentWorkshop.h"
#include "Runtime/turnContext.h"
#include "Core/preferenceStore.h"
#include "Core/logger.h"
#include "Core/messageRouter.h"
#include "Evaluation/conversationEvaluation.h"
#include "Memory/conversationArchive.h"
#include "Memory/conversationRecall.h"
#include "Computer/computerTaskCoordinator.h"
#include "Core/runtimePath.h"
#include "Goals/goalRunner.h"
#include "Goals/goalSandbox.h"
#include "Goals/goalStore.h"
#include "Autonomy/activityExecution.h"
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
#include "Improvement/improvementAgent.h"
#include "Learning/selfAssessment.h"
#include "Perception/activityHistory.h"
#include "Perception/screenAwarenessSchedule.h"
#include "Perception/clipboardText.h"
#include "Perception/windowEventMonitor.h"
#include "Planning/reminders.h"
#include "Performance/performanceRuntime.h"
#include "Presence/presenceRuntime.h"
#include "Presence/webGuestRuntime.h"
#include "Presentation/avatarState.h"
#include "Presentation/debugPresentationSink.h"
#include "Presentation/presentationBus.h"
#include "Skills/skillManager.h"
#include "Runtime/affectController.h"
#include "Runtime/conversationRuntime.h"
#include "Runtime/outputChannelPolicy.h"
#include "Runtime/runtimeEvents.h"
#include "Speech/speechCoordinator.h"
#include "Runtime/sessionResult.h"
#include "Resources/loadGovernor.h"
#include "Resources/resourceMonitor.h"
#include "Resources/resourcePlanner.h"
#include "Speech/speechService.h"
#include "Speech/speechRecognitionService.h"
#include "Speech/addresseeGate.h"
#include "Vision/cameraCaptureService.h"
#include "Vision/screenCaptureService.h"
#include "Vision/visionActionParser.h"
#include "Windows/desktopObserver.h"
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
#include <optional>
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
    // How much of an answer this profile owes. The enum crosses the boundary as
    // itself rather than as a string, so the desktop and the runtime cannot drift
    // into disagreeing about what a mode is called.
    AnswerObligationMode answerObligation = AnswerObligationMode::Balanced;
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

struct ReviaSessionTestAccess;

class ReviaSession
{
    friend struct ReviaSessionTestAccess;

public:
    using ConfirmationHandler = std::function<actions::ConfirmationChoice(
        const actions::ActionRequest&,
        const actions::PolicyDecision&)>;

    // How a typed action result becomes the text the user reads.
    //
    // Public because it is the runtime-truth boundary and that boundary is worth being
    // able to prove. Pure and static: the only input is the outcome, so the profile,
    // its answer obligation, the emotion vector, and the conversation are all
    // structurally incapable of reaching it. Character can style a result everywhere
    // else in the reply; it cannot restate one here, because there is nothing to
    // restate through.
    [[nodiscard]] static std::string FormatActionOutcome(
        const actions::ActionOutcome& outcome);

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
    // Answers a desktop RequireApproval -- the specific human yes that a control like
    // Send needs. Without one installed such a step is refused, as it always was.
    void SetDesktopApprovalHandler(
        revia::policy::DesktopApprovalGate::Handler handler);
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
    // The Windows recording devices present right now, for the shell's picker.
    [[nodiscard]] std::vector<speech::MicrophoneDevice> AvailableMicrophones() const;
    // What the configured device name resolves to, including whether it has gone
    // missing and capture would fall back to the Windows default.
    [[nodiscard]] speech::MicrophoneSelection ResolvedMicrophone() const;
    void SetMicrophoneDevice(const std::string& deviceName);
    // Opens the selected device, records briefly, measures the signal, and optionally
    // transcribes. The transcript is returned for display and never submitted as a
    // conversation turn.
    speech::MicrophoneTestResult TestMicrophone(int seconds = 3, bool transcribe = true);
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
    // Captures one frame from a specific camera.
    //
    // The selection is carried through rather than re-derived: a shell that offered the
    // user a list of cameras has to be able to say which one it meant, and an explicit
    // choice is never satisfied by a different physical device. An empty selection
    // means "the configured preference", which is what autonomous capture uses.
    vision::CameraFrame CaptureCameraFrame(
        bool autonomous = false,
        const vision::CameraSelection& requested = {});
    // The model may locate a target, but it cannot click it. A successful request must
    // resolve to an exact UIA runtime id and then pass through ordinary action policy,
    // confirmation, dispatch, and audit.
    SessionResult ActOnScreen(const std::string& instruction);

    // Karaoke. She performs a song asset from the library folder; nothing here generates
    // singing, and a failure stays inside the performance owner.
    [[nodiscard]] bool StartSong(const std::string& songQuery, std::string& outError);
    void StopSong(const std::string& reason);
    [[nodiscard]] performance::PerformanceStatus SongStatus() const;
    [[nodiscard]] std::vector<performance::SongSummary> Songs() const;
    // Load and mix a song without playing it, to answer "will this one work?" before
    // committing to three minutes of audio.
    [[nodiscard]] performance::SongRehearsal RehearseSong(const std::string& songQuery) const;
    [[nodiscard]] std::string SongListingText() const;
    [[nodiscard]] std::string SongStatusText() const;

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
    // Revia's hands: pointer, keyboard, and starting an approved application. Each is
    // off until the owner turns it on; rawCoordinates and autonomous are narrower
    // authorities that are dropped when the one they sit inside is withdrawn.
    CapabilityUpdateResult SetDesktopControl(
        bool pointer,
        bool keyboard,
        bool applicationLaunch,
        bool rawCoordinates,
        bool visualTargeting,
        bool autonomous,
        actions::CapabilitySettings::DesktopControl::InputScope scope,
        bool allowCommandSurfaces);
    // How much of Revia's in-scope work stops to ask. It never widens which roots,
    // applications, or controls are in scope.
    CapabilityUpdateResult SetExecutionMode(actions::ExecutionMode mode);

    // The emergency stop for synthesized input. It latches, so it is not a pause, and
    // it does not travel through the model, the turn queue, or the action mutex.
    void StopDesktopControl(const std::string& reason);
    CapabilityUpdateResult ResumeDesktopControl();
    [[nodiscard]] bool DesktopControlStopped() const;
    [[nodiscard]] std::string DesktopControlStatus() const;

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
    // The resolved policy for wherever output is going right now.
    [[nodiscard]] runtime::ChannelPolicy CurrentChannelPolicy() const;
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
    speech::VoiceOperationResult RenderVoiceBank(const std::string& presetId);
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
    // Callers hold operationMutex. Startup and active-file edits share application;
    // UI and CLI selection additionally require the preference write to succeed first.
    void ApplyProfileLocked(const std::string& profileId, aiProfile loaded);
    ProfileOperationResult ActivateProfileLocked(const std::string& profileId);
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
    // Whether an action writes into another application's window, which is the only
    // thing that moves the output channel. Foreground application is deliberately not
    // part of this: what Revia is doing decides, not what the user is looking at.
    // Drops the least recently used public channel context when the map is at its
    // bound. The channel passed in is the one being processed and is never evicted.
    void EvictStalePublicContexts(const std::string& keepKey);
    [[nodiscard]] bool IsCompositionAction(const actions::ActionRequest& request) const;
    void BeginExternalComposition(const std::string& application);
    void EndExternalComposition();
    // Submit already holds operationMutex when a /goals command arrives, and that mutex is
    // not recursive, so the command path uses these and the public entry points lock.
    goals::Goal RunGoalUnlocked(goals::Goal goal);
    goals::Goal ResumeGoalUnlocked(const std::string& goalId);
    // The goal work itself, on whatever thread runs it. `reportState` is false for a
    // background task, which must not overwrite the state of a conversation turn.
    goals::Goal ExecuteGoal(goals::Goal goal, std::stop_token stopToken, bool reportState);
    goals::Goal ExecuteResume(const std::string& goalId, std::stop_token stopToken, bool reportState);
    goals::Goal ExecuteOperate(goals::Goal goal, const std::string& request, bool messaging,
        std::stop_token stopToken, bool reportState);
    goals::Goal FinishGoalRun(goals::Goal finished, std::chrono::steady_clock::time_point startedAt,
        bool reportState = true);
    void PublishGoalProgress(const goals::GoalProgress& progress);
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
    // Turns a region a model pointed at into a target with evidence behind it.
    //
    // This is where the strongest available route is chosen, and the order is the point:
    // the UI Automation resolver is tried first, and only a genuine failure to find an
    // element -- not an ambiguous match, and not skipping the attempt -- permits falling
    // back to the region itself. An ambiguous match is left as no target at all rather
    // than downgraded into a claim that UI Automation succeeded.
    //
    // The runtime stamps every piece of evidence from `observation`. Nothing here is
    // read out of model output, because evidence a model could write would be an
    // authorization it granted itself.
    void ResolveVisualTarget(
        actions::ActionRequest& request,
        const actions::windows::DesktopObservation& observation);
    bool TryHandleGoalInput(const std::string& input, SessionResult& result);
    bool TryHandleOperateInput(const std::string& input, SessionResult& result);
    // Shared by /operate and by an ordinary sentence that asked for the same thing, so
    // the two routes cannot drift into different rules.
    bool RunOperateGoal(const std::string& request, SessionResult& result);
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
    std::string ResolveLocalSpeaker(const std::string& input);
    void RecordRelationshipEvidence(
        const std::string& entityId,
        const std::string& userInput,
        const std::string& reply,
        bool succeeded);
    void PersistIdentity();
    void StartStateMaintenance();
    void StopStateMaintenance();
    void RefreshMemoryBackfill();
    // Records what a finished turn says about who she is becoming. Bounded and slow:
    // several consistent observations are needed before anything moves at all.
    void RecordDevelopmentEvidence(const identity::TurnObservation& observation);
    // Moves drives from a confirmed event, alongside the emotional appraisal of it, so
    // wanting and feeling never disagree about what happened.
    void ObserveDrives(const emotion::Stimulus& stimulus);
    // Asks whether there is any reason to act. Called from the initiative loop rather
    // than from a timer of its own: a timer may permit an activity, never motivate one.
    void ConsiderAutonomousActivity(const std::string& triggerReason);
    void RunAutonomousActivity(const autonomy::ActivityDecision& decision,
        const std::string& triggerReason, std::stop_token stopToken = {});
    [[nodiscard]] autonomy::ActivityOutcome ExecuteComputer(
        const autonomy::Activity& activity, const autonomy::ActivityDecision& decision,
        std::stop_token stopToken);
    [[nodiscard]] autonomy::AutonomyEvidence GatherAutonomyEvidence() const;
    [[nodiscard]] autonomy::AutonomyCost GatherAutonomyCost() const;
    // Waits, briefly, for the user to stop typing or moving the mouse before something
    // unprompted is said, the way a person waits for a gap instead of giving up on the
    // thought. Returns the desktop as it stands when the gap came, the wait ran out, or
    // the user spoke to her first.
    // The desktop as the attention policy should see it: the raw sample, plus whether the
    // input clock can be trusted to mean someone is typing.
    [[nodiscard]] initiative::AttentionContext SampleAttention() const;
    [[nodiscard]] initiative::AttentionContext AwaitInputPause(
        std::stop_token stopToken, std::uint64_t inputGeneration) const;
    // Interrupts whatever she chose to do because the user needs attention. Interrupted
    // is not cancelled: what was cut off stays resumable.
    void PreemptAutonomousActivity(const std::string& because);
    // Carries out one decided activity. The scheduler decides; this is the only place
    // that acts, and every externally meaningful step inside it still goes through the
    // ordinary capability, policy, and initiative systems.
    [[nodiscard]] autonomy::ActivityOutcome ExecuteActivity(
        const autonomy::Activity& activity,
        const autonomy::ActivityDecision& decision,
        std::stop_token stopToken = {});
    [[nodiscard]] autonomy::ActivityOutcome ExecuteThink(
        const autonomy::Activity& activity,
        const autonomy::ActivityDecision& decision,
        std::stop_token stopToken = {});
    [[nodiscard]] autonomy::ActivityOutcome ExecuteObserve(
        const autonomy::Activity& activity,
        const autonomy::ActivityDecision& decision,
        std::stop_token stopToken = {});
    [[nodiscard]] autonomy::ActivityOutcome ExecuteResearch(
        const autonomy::Activity& activity,
        const autonomy::ActivityDecision& decision);
    [[nodiscard]] autonomy::ActivityOutcome ExecuteOrganizeMemory(
        const autonomy::Activity& activity,
        const autonomy::ActivityDecision& decision);
    [[nodiscard]] autonomy::ActivityOutcome ExecuteCreate(
        const autonomy::Activity& activity,
        const autonomy::ActivityDecision& decision,
        std::stop_token stopToken = {});
    [[nodiscard]] autonomy::ActivityOutcome ExecuteSpeak(
        const autonomy::Activity& activity,
        const autonomy::ActivityDecision& decision);
    // Whether the activity this worker is running is still the current one. Polled
    // between steps so a long activity yields to the user promptly rather than only at
    // its own boundaries.
    [[nodiscard]] bool ActivityWasInterrupted(const std::string& activityId) const;
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
    // Logs which inference path the voice actually loaded on, once the clone model is
    // resident and the answer is a fact rather than a configuration value.
    void ReportVoiceBackend(const speech::VoiceOperationResult& prepared);
    // Shared by the immediate typed path and the merged voice path. Callers hold
    // operationMutex; the arbiter has already decided what the turn's text is.
    SessionResult RunTurnLocked(const std::string& acceptedInput);
    // What the microphone reports: status, and hands-free transcripts meant for her.
    void OnRecognitionEvent(const speech::RecognitionEvent& recognitionEvent);
    // Runs one turn so that a throw is a failed turn rather than a lost session: the
    // voice drain and adapter loops have no one above them to catch it, and every caller
    // would otherwise be left with busy set and nothing ever clearing it.
    SessionResult GuardTurn(const std::function<SessionResult()>& turn);
    SessionResult RunTurnUnguarded(const std::string& acceptedInput);
    SessionResult ActOnScreenLocked(const std::string& instruction);
    // Runs a background worker's loop until stop is requested. Nothing thrown may leave
    // a worker thread -- an exception escaping a std::jthread ends the process -- so a
    // loop that throws is logged and started again after a short pause.
    void RunBackgroundLoop(
        const char* worker,
        std::stop_token stopToken,
        const std::function<void()>& loop);
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
    // Her review of her own code. Off without a source tree beside the build.
    void StartSelfImprovement();
    void StopSelfImprovement();
    bool HandleImprovementCommand(const std::string& input, SessionResult& result);
    void SignalCuriosity(const std::string& reason);
    void StartExternalAdapterLoop();
    void StopExternalAdapterLoop();
    void QueueExternalAdapterEvent(const presence::ExternalAdapterEvent& event);
    std::stop_token BeginOperation();
    // The token for the operation already in flight. BeginOperation replaces the stop
    // source, so a nested run must not call it: doing so would discard a stop the user
    // requested while the outer operation was still dispatching.
    [[nodiscard]] std::stop_token CurrentOperationToken() const;

    // Background tasks. One runs at a time on its own thread with its own stop source,
    // so she can keep talking while she works and a new message never cancels it.
    bool LaunchTask(const std::string& title,
        std::function<goals::Goal(std::stop_token)> execute, std::string& outMessage);
    void FinishTask(const goals::Goal& finished);
    // Returns false when no task was running.
    bool CancelTask(const std::string& because);
    void StopTaskWorker();
    // Empty when no task is running.
    [[nodiscard]] std::string RunningTaskTitle() const;

    // Reminders and timers: set in plain words, listed and cancelled with /reminders,
    // delivered from PollBackgroundEvents.
    bool TryHandleReminderInput(const std::string& input, SessionResult& result);
    void DeliverDueReminders(planning::WallClock::time_point now);
    // "'stretch' at 3:05 PM (in 12 min); ..." for the state packet. Empty if none.
    [[nodiscard]] std::string DescribeReminders() const;

    // What the user copied, for a turn that asks about it; empty for any other turn.
    // Bounded, marked as untrusted, never saved, and withheld if it holds a credential.
    [[nodiscard]] std::string ClipboardReference(const std::string& input);
    // The goal runner serves one goal at a time: work that needs it while a task holds
    // it is refused here, with a way out. Returns true when it refused.
    bool RefuseWhileTaskRuns(SessionResult& result);
    // The token of the goal executing now: the task's own, or the operation's.
    [[nodiscard]] std::stop_token GoalToken() const;
    // "Working on X (latest: ...)" or a recent result, for the state packet. Empty if none.
    [[nodiscard]] std::string DescribeRunningTask() const;
    [[nodiscard]] std::string DescribeFinishedTask() const;
    // Turn one subsystem's account of what it did into what the session actually does.
    //
    // The only place a TurnEvent becomes a session effect. A subsystem returns events; it
    // does not set state, publish components or reach the event bus, and this is the
    // function that makes that true rather than merely intended.
    SessionResult ApplyTurn(TurnOutcome outcome);

    void SetState(RuntimeState newState, const std::string& activity = "");
    void PublishAffect();
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
    void UpdateResourceLoad(const resources::UsageSnapshot& snapshot);

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
    // A new state has to hold for several consecutive samples before it is adopted.
    // VRAM readings swing hard while models load and free memory -- 111%, then 11%, then
    // 88% within seconds -- and acting on each swing made what Revia would attempt
    // change from one moment to the next for no reason a person could see.
    resources::LoadAdjustment candidateLoad;
    int candidateLoadSamples = 0;
    static constexpr int loadSamplesBeforeAdopting = 3;
    std::optional<autonomy::Activity> runningActivity;
    bool autonomousExecutionActive = false;
    std::stop_source autonomousAttemptStopSource;
    // Rolling counters the scheduler charges against. Kept here rather than in the
    // scheduler so it stays a pure function of its inputs.
    std::deque<std::chrono::steady_clock::time_point> recentActivities;
    std::chrono::steady_clock::time_point lastActivityAt{};
    speech::SpeechService speechService;
    // Who owns the audio channel right now.
    //
    // Every part of Revia that wants to be heard -- a reply, a proposal, a greeting, a
    // song, and later a skill or a game -- goes through this rather than calling
    // SpeechService directly. It does not synthesise anything: the Qwen3-TTS pool below
    // is still the only voice and PerformanceRuntime is still the only thing that plays
    // a song. What this decides is which of them is allowed to make a sound.
    speech::SpeechCoordinator speechCoordinator;
    // Which coordinated intent currently owns the speech backend, so the floor is
    // released by the utterance that actually held it rather than by whatever happens to
    // be active when a late event arrives. Zero when the coordinator started nothing.
    std::atomic<std::uint64_t> speakingIntentId{0};
    speech::SpeechRecognitionService speechRecognitionService;
    // Whether hands-free speech was meant for her: her name, or a follow-up in time.
    speech::AddresseeGate addresseeGate;
    // A separate audio owner with its own device and its own thread. Nothing in here
    // touches the speech queue, so a song that fails to load cannot cost Revia her voice.
    performance::PerformanceRuntime performanceRuntime;
    presence::PresenceRuntime presenceRuntime;
    std::atomic<std::shared_ptr<presence::WebGuestRuntime>> webGuestRuntime;
    // The boundary an avatar will eventually sit behind. Her core publishes what she is
    // doing; a renderer decides what that looks like. The debug sink is the proof the
    // boundary carries enough to draw from, before there is anything to draw.
    presentation::PresentationBus presentationBus;
    std::shared_ptr<presentation::PresentationController> avatar;
    std::shared_ptr<presentation::DebugPresentationSink> presentationDebug;
    // Integrations. They observe and propose; they never execute and never hold
    // authority of their own.
    skills::SkillManager skillManager;
    perception::WindowEventMonitor windowEventMonitor;
    perception::ActivityHistory activityHistory;
    initiative::InitiativeController initiativeController;
    initiative::InputRhythm inputRhythm;
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
    // Eyes for the operator loop. It performs no action and holds no capability: what it
    // sees still reaches the machine only as a typed action through the same policy,
    // confirmation and audit path as everything else.
    actions::windows::DesktopObserver desktopObserver;
    // Everything about deciding what to do next on the machine, in one owner that is
    // not this class.
    //
    // It holds the observation preparation, the decision providers, the payloads the
    // user's words live in, the subgoal lifecycle and the opt-in recorder -- five
    // responsibilities that were spread through this file and share nothing with
    // session lifecycle. This class composes it and asks it questions; it holds no
    // reference back here, starts nothing and executes nothing.
    //
    // Declared after `desktopObserver` because it borrows it, and member initialisation
    // follows declaration order.
    computer::ComputerTaskCoordinator computerTasks{
        desktopObserver,
        core::ResolveRuntimeWritePath(
            std::filesystem::path("RuntimeData") / "ComputerExperience")};
    // Decides when a model role is resident. Declared after `router` because it holds a
    // reference to that router's inventory, and member initialisation follows
    // declaration order.
    //
    // It owns no process. The activator and deactivator installed on it reach back into
    // the server processes this session already owns, so there is still exactly one
    // owner of a llama.cpp child and it is still this class.
    intelligence::ModelLifetimeCoordinator modelLifetime{router.Residency()};
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
    // Set by startup before the save worker runs. Failed loads must never turn
    // a fresh baseline into a replacement for an unreadable identity.
    bool identityPersistenceReady = false;
    std::chrono::milliseconds identitySaveInterval = std::chrono::seconds(30);
    std::chrono::milliseconds emotionSettleInterval = std::chrono::minutes(1);
    std::chrono::milliseconds relationshipQuietInterval = std::chrono::minutes(5);
    std::chrono::milliseconds quietConversationInterval = std::chrono::minutes(20);
    std::jthread stateMaintenanceWorker;
    // The entity whose turn is being handled. Set before a turn runs and read when the
    // state packet is assembled, so relationship state follows whoever is speaking
    // rather than being global.
    mutable std::mutex speakerMutex;
    // Local session attribution only. Adapter authors never replace this selection.
    std::string currentSpeakerId = identity::LocalUserEntityId();
    learning::SelfAssessmentEngine selfAssessment;
    // Declared after the router, bus, logger, and assessment its callbacks use, so it is
    // destroyed -- and its thread joined -- before any of them.
    std::shared_ptr<improvement::ProposalStore> improvementStore =
        std::make_shared<improvement::ProposalStore>();
    improvement::ImprovementAgent improvementAgent;
    visual::DiagramStore diagramStore;
    visual::ImageGenerator imageGenerator;
    // The working document lives in here now, with the five operations that touch it.
    // Declared after every collaborator it borrows, so it is destroyed before them.
    DocumentWorkshop documentWorkshop;
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
    // Public history is deliberately channel-scoped and memory-only. It never enters
    // the local user's conversationContext or durable conversation archive.
    std::unordered_map<std::string, std::deque<conversationMessage>>
        publicConversationContexts;
    // Last-used ordinal per channel, for LRU eviction. A monotonic counter rather than a
    // clock: ordering is all this needs, and a counter cannot go backwards.
    std::unordered_map<std::string, std::uint64_t> publicContextLastUsed;
    std::uint64_t publicContextClock = 0;
    std::condition_variable_any initiativeCondition;
    std::condition_variable_any curiosityCondition;
    std::uint64_t initiativeSignalVersion = 0;
    std::string initiativeSignalReason;
    std::uint64_t curiositySignalVersion = 0;
    std::string curiositySignalReason;
    std::stop_source curiosityAttemptStopSource;
    outputChannel outputTarget = outputChannel::LocalVoice;
    // What to go back to when a composition ends. Depth-counted because one act of
    // composing is often two actions -- set the text, then click send -- and restoring
    // after the first would put Revia back on local voice halfway through.
    outputChannel previousOutputTarget = outputChannel::LocalVoice;
    std::string previousOutputApplication;
    int compositionDepth = 0;
    std::string outputApplication;
    std::stop_source activeStopSource;
    ConfirmationHandler confirmationHandler;
    // Loads the assigned Qwen3-TTS voice alongside the remaining startup stages.
    std::jthread voiceWarmupWorker;
    std::atomic<bool> voiceWarmupFinished = true;
    // Whether the load is still wanted. Separate from `started`, because the warmup now
    // begins while startup is still running and `started` is deliberately false until
    // every stage has finished -- reading it there would abandon the load immediately.
    std::atomic<bool> voiceWarmupWanted = false;
    // When background awareness should look, and what it last saw. The thread
    // below stays here because the capture needs the foreground lock, the busy
    // flag and the backend; the scheduling state does not, and was six members of
    // this class that nothing else touched.
    perception::ScreenAwarenessSchedule screenAwareness;
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
    struct BackgroundTask
    {
        std::string title;
        std::chrono::steady_clock::time_point startedAt;
        std::string progress;
    };
    struct TaskReport
    {
        std::string summary;
        goals::GoalStatus status = goals::GoalStatus::Planned;
        std::chrono::steady_clock::time_point finishedAt;
    };
    planning::ReminderBook reminders;
    std::function<std::optional<perception::ClipboardText>()> clipboardReader =
        [] { return perception::ReadClipboardText(6000); };
    // Serialises launching against launching and stopping. The worker never takes it.
    std::mutex taskLaunchMutex;
    mutable std::mutex taskMutex;
    std::optional<BackgroundTask> activeTask;
    std::optional<TaskReport> lastTaskReport;
    std::stop_source taskStopSource;
    // Set while a goal executes, so its callbacks honour that goal's stop.
    std::optional<std::stop_token> executingGoalToken;
    std::jthread taskWorker;
    // Counts launches, so a caller can tell whether its request started a task.
    std::atomic<std::uint64_t> tasksLaunched{0};
    struct GoalTokenScope
    {
        GoalTokenScope(ReviaSession& session, std::stop_token token);
        ~GoalTokenScope();
        GoalTokenScope(const GoalTokenScope&) = delete;
        GoalTokenScope& operator=(const GoalTokenScope&) = delete;
        ReviaSession& session;
    };
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
