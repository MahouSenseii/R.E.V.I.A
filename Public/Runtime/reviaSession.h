#pragma once

#include "Actions/actionRuntime.h"
#include "Agents/turnCoordinator.h"
#include "Core/commandManager.h"
#include "Core/configManager.h"
#include "Core/conversationContext.h"
#include "Core/logger.h"
#include "Core/messageRouter.h"
#include "Goals/goalRunner.h"
#include "Goals/goalSandbox.h"
#include "Goals/goalStore.h"
#include "LLM/LLamaCPP/llamaCppServerProcess.h"
#include "Initiative/initiativeController.h"
#include "Learning/learningReview.h"
#include "Perception/activityHistory.h"
#include "Perception/windowEventMonitor.h"
#include "Runtime/affectController.h"
#include "Runtime/runtimeEvents.h"
#include "Speech/speechService.h"
#include "Speech/speechRecognitionService.h"
#include "Vision/screenCaptureService.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace revia::runtime
{

struct SessionResult
{
    bool succeeded = true;
    bool shouldExit = false;
    // Set by the CLI streaming path: tokens were already printed live to the terminal.
    // Do not use it to decide what the desktop shell has shown -- it is true for every
    // llama.cpp reply, streamed to the UI or not.
    bool wasStreamed = false;
    // Set only when this reply was published sentence by sentence as it was spoken, so
    // the shell has already displayed all of it and must not append it a second time.
    bool spokenAsFragments = false;
    // What Revia did to produce this reply: her posture, any reasoning the model emitted,
    // and where the time went. Shown collapsed in the shell so it is available without
    // being in the way.
    std::string reasoning;
    bool fromAssistant = false;
    std::string text;
    std::string reason;
    // Set when this reply was handed to the speech worker. The shell holds the text until
    // the matching Speaking event so the words appear with the voice rather than well
    // ahead of it. A reply that will not be spoken leaves this false and is shown at once.
    bool speechPending = false;
    std::uint64_t utteranceId = 0;
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
    SessionResult Submit(const std::string& input);
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
    bool BeginListening();
    bool EndListening();
    SessionResult AnalyzeScreen(const std::string& prompt);

    // Stage 4. The runner itself adds no authority: every step goes through the same
    // dispatcher, policy, and audit path as an interactive action, and has to prove it
    // happened before the goal advances.
    goals::Goal RunGoal(goals::Goal goal);
    goals::Goal ResumeGoal(const std::string& goalId);
    [[nodiscard]] std::vector<goals::Goal> RecentGoals(std::size_t maxGoals = 25) const;
    [[nodiscard]] std::vector<goals::Goal> ResumableGoals() const;

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
    void StartVoiceWarmup();
    void StopVoiceWarmup();
    // Its own thread, not the shell's poll timer. A companion that only considers speaking
    // while a debug window happens to be open is not a companion.
    void StartInitiativeLoop();
    void StopInitiativeLoop();
    std::stop_token BeginOperation();
    // The token for the operation already in flight. BeginOperation replaces the stop
    // source, so a nested run must not call it: doing so would discard a stop the user
    // requested while the outer operation was still dispatching.
    [[nodiscard]] std::stop_token CurrentOperationToken() const;
    void SetState(RuntimeState newState, const std::string& activity = "");
    void PublishAffect(const AffectSnapshot& affect);
    void Publish(RuntimeEventKind kind, const std::string& message, std::uint64_t turnId = 0) const;

    RuntimeEventBus eventBus;
    logger appLogger;
    messageRouter router;
    configManager config;
    commandManager commands;
    conversationContext context;
    appSettings settings;
    aiProfile profile;
    actions::ActionRuntime actionRuntime;
    // Declared after actionRuntime: GoalRunner holds references to both of these.
    goals::GoalStore goalStore;
    goals::GoalRunner goalRunner;
    AffectController affectController;
    speech::SpeechService speechService;
    speech::SpeechRecognitionService speechRecognitionService;
    perception::WindowEventMonitor windowEventMonitor;
    perception::ActivityHistory activityHistory;
    initiative::InitiativeController initiativeController;
    vision::ScreenCaptureService screenCaptureService;
    llamaCppServerProcess llamaServerProcess;
    llamaCppServerProcess embeddingServerProcess;
    agents::TurnCoordinator turnCoordinator;

    mutable std::mutex operationMutex;
    mutable std::mutex cancellationMutex;
    mutable std::mutex confirmationMutex;
    mutable std::mutex voiceStudioMutex;
    mutable std::mutex channelMutex;
    outputChannel outputTarget = outputChannel::LocalVoice;
    std::string outputApplication;
    std::stop_source activeStopSource;
    ConfirmationHandler confirmationHandler;
    // Loads the assigned Qwen3-TTS voice after startup has already reported ready.
    std::jthread voiceWarmupWorker;
    std::atomic<bool> voiceWarmupFinished = true;
    std::jthread initiativeWorker;
    std::atomic<RuntimeState> state = RuntimeState::Offline;
    std::atomic<bool> started = false;
    std::atomic<bool> busy = false;
    bool llmAvailable = false;
    std::uint64_t turnCounter = 0;
    std::uint64_t utteranceCounter = 0;
};

} // namespace revia::runtime
