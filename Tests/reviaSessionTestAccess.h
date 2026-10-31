#pragma once
#include "speakerRecognitionTestAccess.h"
#include "Speech/speakerEnrollmentDialogue.h"

#include "Computer/computerSettings.h"
#include "Core/profile.h"
#include "LLM/endpointSettings.h"
#include "LLM/responseTypes.h"
#include "Memory/memoryTypes.h"
#include "testSupport.h"
#include "Runtime/reviaSession.h"

#include "Computer/legacyLlmPolicy.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace revia::runtime
{

struct ReviaSessionTestAccess
{
    static void ConfigureEditorFixture(ReviaSession& session)
    {
        session.settings.localApi.bEnabled = session.settings.localApi.bTrustedPrivate = true;
        session.settings.perception.bEnabled = true;
        session.settings.initiative.bEnabled = true;
        session.settings.initiative.cooldownSeconds = 0;
        session.settings.initiative.quietInputSeconds = 0;
        session.settings.initiative.bSuppressWhenFullScreen = false;
        session.initiativeController.Configure(session.settings.initiative);
        session.conversationStarter.Configure(session.settings.initiative);
        session.started.store(true);
        session.actionRuntime.BindAuthority(session.companionAuthority, session.Stamp());
        session.InitializeTaskContracts();
    }
    static std::vector<initiative::StarterCue> EditorCues(ReviaSession& session)
    { return session.conversationStarter.RecentCues(std::chrono::system_clock::now()); }
    static std::string EditorContext(const ReviaSession& session) { return session.CurrentEditorContext(); }
    static bool EditorSourceAvailable(const ReviaSession& session)
    {
        return session.CurrentSavedFileContext().available;
    }
    static bool PrepareEditorOpeningContext(ReviaSession& session, const bool admitted = true)
    {
        std::lock_guard lock(session.operationMutex);
        const auto scope = session.CapturePrivateMemoryScope();
        const auto origin = session.Stamp();
        return session.PreparePrivateContext(scope, [&]
            { return admitted && session.started.load() && session.sessionIdentity.IsCurrent(origin); });
    }
    static void RecordDeliveredEditorOpening(ReviaSession& session, const std::string& text)
    {
        std::lock_guard lock(session.operationMutex);
        session.context.AddMessage("assistant", text);
    }
    static void RetireEditorInput(ReviaSession& session) { ++session.userInteractionGeneration; }
    static void EditorFocusMode(ReviaSession& session) { session.settings.initiative.mode = "focus"; }
    static std::stop_token ArmOptionalEditorAttempt(ReviaSession& session)
    {
        auto source = std::make_shared<std::stop_source>();
        session.editorOpeningStop.store(source);
        session.busy.store(true);
        return source->get_token();
    }
    static void ReleaseOptionalEditorAttempt(ReviaSession& session)
    { session.editorOpeningStop.store({}); session.busy.store(false); }
    static void ExpireEditorSnapshot(ReviaSession& session)
    { session.ExpireEditorObservation(std::chrono::system_clock::now() + std::chrono::seconds{121}); }
    static void ExpireEditorSnapshotAt(ReviaSession& session, const std::chrono::system_clock::time_point now)
    { session.ExpireEditorObservation(now); }
    static std::stop_token BeginEditorRetirementFixture(ReviaSession& session, const RuntimeState state)
    {
        std::lock_guard lock(session.operationMutex);
        const auto token = session.BeginOperation();
        session.state.store(state);
        return token;
    }
    static void RetireEditorOpening(ReviaSession& session, const std::stop_token token,
        const RuntimeStamp& origin, const identity::AudienceContext& audience)
    {
        std::lock_guard lock(session.operationMutex);
        session.RetireOptionalOpeningLocked(token, origin, audience);
    }
    static void ChangeEditorRetirementAudience(ReviaSession& session)
    { std::lock_guard lock(session.audienceMutex); ++session.configuredAudience.revision; }
    static void RestartEditorRetirementSession(ReviaSession& session) { session.sessionIdentity.BeginSession(); }
    static initiative::InitiativeController::Consideration ReserveEditorCue(ReviaSession& session, const initiative::StarterCue& cue)
    {
        initiative::InitiativeController::Evidence evidence;
        evidence.conversationCues.push_back(cue);
        return session.initiativeController.Consider(evidence, {});
    }
    static void SetEditorLoadAdjustment(ReviaSession& session, const resources::LoadState state, const bool allowNewWork)
    {
        std::lock_guard lock(session.loadMutex);
        session.currentLoad.state = state;
        session.currentLoad.allowOptionalBackgroundWork = allowNewWork;
    }
    static std::function<bool()> EditorDeliveryAdmission(ReviaSession& session, const initiative::StarterCue& cue,
        const bool requireSpareResources = true, const initiative::AttentionContext attention = {})
    {
        initiative::Proposal proposal;
        proposal.kind = initiative::Proposal::Kind::ConversationStarter;
        proposal.admission = cue.admission;
        proposal.optionalObservation = cue.optionalObservation;
        return session.CaptureInitiativeAdmission(proposal, session.Stamp(), session.Audience(),
            session.userInteractionGeneration.load(), session.editorObservationEpoch.load(), requireSpareResources,
            [attention] { return attention; });
    }
    static SessionResult RunAcceptedTurn(ReviaSession& session, const agents::InputBatch& batch)
    {
        std::lock_guard lock(session.operationMutex);
        session.llmAvailable = session.router.CheckLLMHealth().bIsAvailable;
        (void)session.BeginOperation();
        return session.RunTurnLocked(batch);
    }
    static ResponseLatencySnapshot ResponseTiming(const ReviaSession& session)
    {
        return session.conversationRuntime.ResponseLatencies();
    }
    static bool InitializeQualityAssessment(ReviaSession& session, const std::filesystem::path& path, std::string& error)
    {
        return session.selfAssessment.Initialize(path, error);
    }
    static void DisableQualityFixtureEmbedding(ReviaSession& session)
    {
        embeddingSettings embedding;
        embedding.bEnabled = embedding.bAutoStartServer = false;
        embedding.modelName.clear();
        session.router.ApplyLLMSettings(session.settings.llm, embedding, session.profile);
    }
    static memory::MemoryScope CorrectionScope(const ReviaSession& session)
    { return session.CapturePrivateMemoryScope(); }
    static void QueueMemoryCorrections(ReviaSession& session, const std::vector<memory::MemoryCorrectionProposal>& proposals)
    { session.QueueMemoryCorrectionProposals(proposals, session.Stamp()); }
    static void QueueMemoryCorrectionsFromOrigin(ReviaSession& session,
        const std::vector<memory::MemoryCorrectionProposal>& proposals, const RuntimeStamp& origin)
    {
        session.QueueMemoryCorrectionProposals(proposals, origin);
    }
    static void MarkStudioStarted(ReviaSession& session, bool value)
    {
        session.started.store(value);
    }
    static attachments::AttachmentLibrary& Attachments(ReviaSession& session)
    {
        return session.Attachments();
    }
    static void ConfigureGeneratedMediaRuntime(ReviaSession& session)
    {
        session.settings.embedding.bEnabled = false;
        session.started.store(true);
    }
    static void DiscardGeneratedMedia(ReviaSession& session, const conversationMediaReference& reference)
    {
        session.DiscardGeneratedMedia({reference});
    }
    static std::vector<std::string> PendingGeneratedInvalidations(ReviaSession& session)
    {
        std::lock_guard lock(session.attachmentMutex);
        return session.forgottenMedia;
    }
    static void SeedGeneratedContext(ReviaSession& session, const conversationMediaReference& reference, const std::string& text)
    {
        std::lock_guard lock(session.operationMutex);
        const auto scope = session.CapturePrivateMemoryScope();
        conversationMessage message{"assistant", text, scope.participantId, scope};
        message.mediaReferences = {reference};
        session.context.AddMessage(std::move(message));
    }
    static bool RecordGeneratedArchive(
        ReviaSession& session, const conversationMediaReference& reference, const std::string& text, std::string& error)
    {
        std::lock_guard lock(session.operationMutex);
        return session.conversationArchive.RecordGeneratedAssistant(
            session.CurrentConversationId(), text, {reference}, error, session.CapturePrivateMemoryScope(), [] { return true; });
    }
    static void DrainGeneratedInvalidations(ReviaSession& session)
    {
        std::lock_guard lock(session.operationMutex);
        session.DrainForgottenMedia();
    }
    static bool SetGeneratedQuiet(ReviaSession& session, const bool quiet)
    {
        return session.initiativeController.RequestQuiet(quiet ? "quiet please" : "you can talk now");
    }
    static bool GeneratedSpeechAdmitted(ReviaSession& session)
    {
        return session.CaptureSpeechAdmission(session.Audience(), session.Stamp())();
    }
    static void RetireGeneratedConversation(ReviaSession& session)
    {
        session.conversationEpoch.fetch_add(1);
    }
    static std::function<void()> ArmGeneratedTaskCompletion(ReviaSession& session, std::optional<conversationMediaReference> reference = {})
    {
        const auto context = session.CaptureTaskContext();
        const auto origin =
            session.sessionIdentity.Stamp(actions::NewActionId(), actions::NewActionId(), session.companionAuthority->Revision());
        tests::Check(session.companionAuthority->RegisterTask(origin, {}, policy::AuthorityPermissions::WithinMachineCeiling(), false),
            "The generated task fixture could not register its native task scope.");
        {
            std::lock_guard lock(session.taskMutex);
            tests::Check(!session.activeTask, "The generated task fixture replaced a live task.");
            session.activeTask = ReviaSession::BackgroundTask{"Generated media fixture", std::chrono::steady_clock::now(), "completed",
                origin, context, true, std::move(reference), session.conversationEpoch.load()};
        }
        return [&session, origin, context]
        {
            goals::Goal finished;
            finished.title = "Generated media fixture";
            finished.status = goals::GoalStatus::Succeeded;
            session.FinishTask(finished, origin, context);
        };
    }
    static SessionResult SubmitMediaWithAdmission(ReviaSession& session, const std::string& input,
        std::vector<conversationMediaReference> references, std::function<bool()> admission)
    {
        return session.SubmitTurn(input, agents::InputSource::Typed, {}, std::move(references), std::move(admission));
    }
    // Main with or without its vision projector, and no embedding server, so document
    // search is lexical and deterministic.
    static void ConfigureAttachments(ReviaSession& session, bool vision)
    {
        session.settings.llm.bVisionEnabled = vision;
        session.settings.embedding.bEnabled = false;
        session.router.ApplyLLMSettings(
            session.settings.llm, session.fastLlmSettings, session.expertLlmSettings, embeddingSettings{}, aiProfile{}, true, true);
    }
    static void UseVirtualMachine(ReviaSession& session, std::shared_ptr<workspace::VirtualMachineRuntime> machine)
    { session.virtualMachine = std::move(machine); }
    static void StartVirtualMachine(ReviaSession& session)
    { std::lock_guard lock(session.operationMutex); session.StartVirtualMachine(); }
    static void SetVoiceWarmupFinished(ReviaSession& session, bool value)
    { session.voiceWarmupFinished.store(value); }
    static void SetVoiceWarmupSucceeded(ReviaSession& session, bool value)
    { session.voiceWarmupSucceeded.store(value); }
    static actions::CapabilitySettings GoalScope(const ReviaSession& session)
    { return session.DeriveGoalScope(); }
    static void WithGoalAdmission(ReviaSession& session, const std::function<void(const RuntimeStamp&, bool)>& examine,
        const std::stop_token stopToken = {})
    {
        ReviaSession::GoalTokenScope scope(session, stopToken);
        tests::Check(scope.admitted, "The contract fixture could not register its goal.");
        examine(scope.stamp, session.OperatorAdmitted(stopToken));
    }
    static std::shared_ptr<const core::TaskContract> ActionContract(ReviaSession& session,
        const actions::ActionRequest& request, const RuntimeStamp& stamp)
    { return session.BuildActionTaskContract(request, stamp, {}); }
    static std::string ContractRefusal(const ReviaSession& session, const core::TaskContract& contract)
    { return session.TaskContractRefusal(contract, {}); }
    static std::shared_ptr<const core::TaskContract> ForegroundContract(ReviaSession& session)
    {
        agents::InputBatch batch;
        batch.context = session.CaptureInputContext(agents::InputSource::Typed);
        batch.text = "Foreground contract fixture";
        return session.BuildTurnTaskContract(batch);
    }
    static agents::InputContext StudioInput(ReviaSession& session, agents::InputSource source, identity::SpeakerObservation speaker = {})
    { return session.CaptureInputContext(source, speaker); }
    static bool StudioInputCurrent(const ReviaSession& session, const agents::InputContext& context)
    { return session.InputContextCurrent(context); }
    static bool RegisterStudioTask(ReviaSession& session, RuntimeStamp stamp)
    { return session.companionAuthority->RegisterTask(stamp, {}); }
    static speech::SpeechService& Speech(ReviaSession& session) { return session.speechService; }
    static diagnostics::IssueLog& SpeechIssues(ReviaSession& session) { return session.speechIssues; }
    static void SpeechEvent(ReviaSession& session, const speech::SpeechEvent& event) { session.HandleSpeechEvent(event); }
    static void LoadSpeechFaultHistory(ReviaSession& session) { session.LoadSpeechFaultHistory(); }
    static void SetSpeakingIntent(ReviaSession& session, std::uint64_t id) { session.speakingIntentId.store(id); }
    static std::uint64_t SpeakingIntent(const ReviaSession& session) { return session.speakingIntentId.load(); }
    static actions::ActionRuntime& Actions(ReviaSession& session) { return session.actionRuntime; }
    static SessionResult RunStudioAction(ReviaSession& session, actions::ActionRequest request)
    {
        std::lock_guard lock(session.operationMutex);
        (void)session.BeginOperation();
        return session.ExecuteAction(std::move(request));
    }
    static void JoinWorkflow(ReviaSession& session) { session.agentWorkflow.Join(); }
    static void SetDelegationMode(ReviaSession& session, agents::DelegationMode mode) { session.delegationMode.store(mode); }
    // Ends only the report loop and joins its current tail, without cancelling the record
    // or invalidating the session. Publication callbacks may have just changed that record.
    static void JoinDelegationReportWorker(ReviaSession& session) { session.StopDelegationWorker(); }
    // Simulates an already resolved, explicitly private consented input. No microphone,
    // recognizer or biometric match is claimed by this contract fixture.
    static agents::InputBatch ConsentedDelegationInput(ReviaSession& session, const std::string& text)
    {
        agents::InputBatch batch;
        batch.source = agents::InputSource::Voice;
        batch.text = text;
        batch.context = session.CaptureInputContext(agents::InputSource::Typed);
        (void)session.relationships.Get(batch.context.participantId);
        std::string error;
        tests::Check(session.relationships.GrantRecognitionConsent(batch.context.participantId, error), error);
        const auto revision = session.relationships.RecognitionConsentRevision(batch.context.participantId);
        tests::Check(revision.has_value(), "The delegated report fixture has no recognition consent.");
        batch.context.participantSource = identity::SpeakerSource::ConsentedVoice;
        batch.context.consentRevision = *revision;
        return batch;
    }
    static void RevokeDelegationConsent(ReviaSession& session, const std::string& participant)
    {
        std::string error;
        tests::Check(session.relationships.RevokeRecognitionConsent(participant, error), error);
    }
    static std::shared_ptr<const core::TaskContract> DelegationContract(ReviaSession& session)
    {
        std::lock_guard lock(session.delegationMutex);
        return session.delegation ? session.delegation->taskContract : nullptr;
    }
    static std::vector<memory::ArchivedTurn> RawArchivedSession(ReviaSession& session, const std::string& id)
    { return session.conversationArchive.LoadSession(id); }
    // A typed command as a turn would handle it, for a caller that already holds the lane.
    static SessionResult HandleCommand(ReviaSession& session, const std::string& input)
    {
        SessionResult result;
        result.succeeded = true;
        (void)session.TryHandleActionInput(input, result);
        return result;
    }
    static void Log(ReviaSession& session, const std::string& text) { session.appLogger.Log(text); }
    static bool LoadConfiguredProfile(const ReviaSession& session, const std::string& id, aiProfile& profile)
    { return session.config.LoadProfile(id, profile); }
    static agents::NodeResult RunAgentProvider(ReviaSession& session, const agents::NodeRequest& request)
    {
        return session.AgentProvider(AgentProviderMode::Local)(request, {});
    }
    static void OpenConversationArchive(ReviaSession& session, const std::filesystem::path& path, const std::string& sessionId)
    {
        std::lock_guard lock(session.operationMutex);
        session.settings.conversation.bArchiveEnabled = true;
        session.conversationArchive = memory::ConversationArchive(path.string());
        std::string error;
        tests::Check(session.conversationArchive.BeginSession(sessionId, error), error);
        std::lock_guard idLock(session.conversationIdMutex);
        session.conversationSessionId = sessionId;
    }
    static std::string RecallArchivedConversation(ReviaSession& session, const memory::RecallRequest& request,
        const std::string& currentInput, const memory::MemoryScope& scope)
    {
        std::lock_guard lock(session.operationMutex);
        (void)session.BeginOperation();
        return session.RecallConversation(request, currentInput, scope);
    }
    static std::vector<conversationMessage> WorkingContext(ReviaSession& session)
    {
        std::lock_guard lock(session.operationMutex);
        return session.context.GetRecentMessages();
    }
    // No fixture serves the embedding port. Left enabled, every model step asks it first, and
    // on Windows a refused loopback connect takes about two seconds.
    static embeddingSettings NoEmbeddingServer()
    {
        embeddingSettings embedding;
        embedding.bEnabled = false;
        return embedding;
    }
    static void ConfigureStartupBrains(ReviaSession& session, int port, bool fastWarm = true, bool expertWarm = true,
        const embeddingSettings& embedding = embeddingSettings{})
    {
        session.settings.llm.backend = "LLamaCpp";
        session.settings.llm.host = "127.0.0.1";
        session.settings.llm.port = port;
        session.settings.llm.modelName = "fixture-main";
        session.settings.llm.bAutoStartServer = false;
        session.settings.llm.bVisionEnabled = false;
        session.fastLlmSettings = session.expertLlmSettings = session.settings.llm;
        session.settings.intelligence.fast.bWarmAtStartup = fastWarm;
        session.settings.intelligence.expert.bWarmAtStartup = expertWarm;
        if (!embedding.bEnabled)
            session.settings.embedding.bEnabled = false;
        session.router.ApplyLLMSettings(session.settings.llm, session.fastLlmSettings,
            session.expertLlmSettings, embedding, aiProfile{}, true, true);
    }
    // As the shipped settings run on one GPU: Expert is not configured, so background reviews go
    // straight to Main and share its slots with chat.
    static void ConfigureMainOnly(ReviaSession& session, int port, const embeddingSettings& embedding = embeddingSettings{})
    {
        ConfigureStartupBrains(session, port, true, true, embedding);
        session.router.ApplyLLMSettings(session.settings.llm, session.fastLlmSettings,
            session.expertLlmSettings, embedding, aiProfile{}, true, false);
    }
    static bool EnsureBrain(ReviaSession& session, intelligence::IntelligenceTier tier, std::stop_token token = {})
    {
        if (tier == intelligence::IntelligenceTier::Fast) return session.EnsureFastBrainAvailable(token);
        if (tier == intelligence::IntelligenceTier::Expert) return session.EnsureExpertBrainAvailable(token);
        return session.EnsureLLMAvailable(token);
    }
    static intelligence::ResidencyState BrainState(const ReviaSession& session, intelligence::IntelligenceTier tier)
    {
        for (const auto& model : session.router.ModelResidencySnapshot())
            if (model.tier == tier) return model.state;
        throw std::runtime_error("Missing brain residency fixture.");
    }
    static void ApplyBrainProfile(ReviaSession& session) { session.router.ApplyProfile(aiProfile{}); }
    static void VirtuallyUnloadBrain(ReviaSession& session, intelligence::IntelligenceTier tier)
    { session.router.Residency().MarkUnloaded(tier, "External endpoint retained by fixture."); }
    static bool InitiativeWorkersRunning(const ReviaSession& session)
    { return session.initiativeWorker.joinable() && session.curiosityWorker.joinable(); }
    static void PlannedMainDevice(ReviaSession& session, const std::string& device)
    { session.settings.llm.device = device; }
    // What the conversation runtime's observer does as a private reply is delivered.
    static void HoldDeliveredReply(ReviaSession& session, const responseOutput& reply, const std::string& participantId)
    {
        const auto revision = session.Audience().revision;
        std::lock_guard operationLock(session.operationMutex);
        session.HoldDeliveredReply(reply, revision, participantId);
    }
    static std::string PlannedMainDevice(const ReviaSession& session)
    { return session.settings.llm.device; }

    static SessionResult GuardTurn(ReviaSession& session, const std::function<SessionResult()>& turn)
    { return session.GuardTurn(turn); }
    static void MarkBusy(ReviaSession& session) { session.busy.store(true); }
    static bool LaunchTask(ReviaSession& session, const std::string& title,
        std::function<goals::Goal(std::stop_token)> execute, std::string& outMessage)
    { return session.LaunchTask(title, std::move(execute), outMessage); }
    static void WaitForTask(ReviaSession& session)
    {
        std::jthread worker;
        {
            std::lock_guard lock(session.taskMutex);
            worker = std::move(session.taskWorker);
        }
        if (worker.joinable()) worker.join();
    }
    static std::string RunningTask(const ReviaSession& session) { return session.DescribeRunningTask(); }
    static std::string FinishedTask(const ReviaSession& session) { return session.DescribeFinishedTask(); }
    static std::string IntroduceTaskSpeaker(ReviaSession& session, const std::string& name)
    {
        auto captured = session.CaptureInputContext(agents::InputSource::Typed);
        return session.ResolveLocalSpeaker("my name is " + name, captured);
    }
    static void TaskProgress(ReviaSession& session, const goals::GoalProgress& progress)
    { session.PublishGoalProgress(progress); }
    static void DeliverReminders(ReviaSession& session, planning::WallClock::time_point now)
    { session.DeliverDueReminders(now); }
    static bool InitializeReminders(ReviaSession& session, const std::filesystem::path& path, std::string& error) { return session.reminders.Initialize(path, error); }
    static std::string Reminders(const ReviaSession& session) { return session.DescribeReminders(); }
    static void SetClipboard(ReviaSession& session, std::function<std::optional<perception::ClipboardText>()> reader)
    { session.clipboardReader = std::move(reader); }
    static std::string ClipboardReference(ReviaSession& session, const std::string& input)
    { return session.ClipboardReference(input); }
    static void Hear(ReviaSession& session, const speech::RecognitionEvent& event)
    { session.OnRecognitionEvent(event); }
    static std::string TakeOfferedInput(ReviaSession& session) { return session.inputArbiter.Take(); }
    static bool IsBusy(const ReviaSession& session) { return session.busy.load(); }
    static void RunBackgroundLoop(ReviaSession& session, std::stop_token stopToken, const std::function<void()>& loop)
    { session.RunBackgroundLoop("Test worker", stopToken, loop); }
    static emotion::EmotionRuntime& Emotions(ReviaSession& session) { return session.emotionRuntime; }
    static AffectController& LegacyAffect(ReviaSession& session) { return session.affectController; }
    static identity::RelationshipRegistry& People(ReviaSession& session) { return session.relationships; }
    static bool CaptureEnrollmentSample(ReviaSession& session, const std::filesystem::path& wave, std::string& message)
    { return session.CaptureSpeakerEnrollmentSample(wave, {}, message); }
    static void BindStudioSpeakerResolver(ReviaSession& session) { session.ConfigureSpeakerResolver(); }
    static identity::SpeakerObservation ResolveStudioVoice(ReviaSession& session, const std::filesystem::path& wave)
    { return speech::SpeakerRecognitionTestAccess::Resolve(session.speechRecognitionService, wave); }
    static bool HandleStudioEnrollment(ReviaSession& session, const agents::InputBatch& input, SessionResult& result)
    { return session.TryHandleSpeakerEnrollment(input, result); }
    static bool StudioEnrollmentPending(ReviaSession& session)
    { return session.speakerEnrollment && session.speakerEnrollment->Snapshot().phase != speech::SpeakerEnrollmentPhase::Idle; }

    static float PendingDevelopment(ReviaSession& session, identity::Trait trait)
    { return session.developmentEngine.PendingEvidence(trait); }
    static void RecordRelationship(ReviaSession& session, const std::string& entityId, const std::string& input,
        const std::string& reply, const bool succeeded)
    { session.RecordRelationshipEvidence(entityId, input, reply, succeeded); }

    static void SampleLoad(ReviaSession& session, const resources::UsageSnapshot& usage)
    { session.UpdateResourceLoad(usage); }

    static void StartIdleReviewFixture(ReviaSession& session)
    {
        session.settings.initiative.bEnabled = true;
        session.settings.initiative.bCuriosityEnabled = true;
        session.settings.initiative.bSpontaneousSpeechEnabled = false;
        session.settings.initiative.curiosityCheckSeconds = 1;
        session.settings.initiative.autonomousQuietSeconds = 0;
        session.StartCuriosityLoop();
    }

    static void UsePreferences(ReviaSession& session, const std::filesystem::path& root)
    { session.preferenceStore = core::PreferenceStore(root / "preferences.json"); }
    // State-only screen-cache fixture: no window monitor or capture worker is started.
    static void SetSyntheticPerceptionEnabled(ReviaSession& session, const bool enabled)
    {
        std::lock_guard lock(session.preferenceSnapshotMutex);
        session.settings.perception.bEnabled = enabled;
    }
    static void RecordScreenObservation(ReviaSession& session, const std::string& text) { session.screenAwareness.Record(text); }
    static std::string CachedScreenObservation(const ReviaSession& session) { return session.screenAwareness.LatestObservation().text; }
    static std::string ScreenContext(const ReviaSession& session) { return session.CurrentScreenContext(); }
    static bool ScreenAwarenessRunning(const ReviaSession& session) { return session.screenAwarenessWorker.joinable(); }
    // In a call only her name counts, so no earlier exchange opens a follow-up window.
    static bool AcceptsUnnamedSpeech(ReviaSession& session, const std::string& transcript)
    { return session.addresseeGate.Accept(transcript, speech::AddresseeGate::Clock::now(), true); }
    static std::string CloudUseFor(const ReviaSession& session) { return session.router.Cloud().Settings().useFor; }
    static std::optional<bool> McpServerEnabled(const ReviaSession& session, const std::string& id)
    {
        for (const auto& server : session.mcpManager.Status())
            if (server.id == id) return server.enabled;
        return std::nullopt;
    }

    static void StopIdleReviewFixture(ReviaSession& session) { session.StopCuriosityLoop(); }
    static void RunIdleActivity(ReviaSession& session, const autonomy::ActivityDecision& decision)
    { session.RunAutonomousActivity(decision, "fixture nomination"); }
    static void AgeIdleBudget(ReviaSession& session)
    {
        std::lock_guard lock(session.autonomyMutex);
        session.lastActivityAt = std::chrono::steady_clock::now() - std::chrono::hours(2);
        for (auto& at : session.recentActivities) at = session.lastActivityAt;
    }

    static void MaintenanceEvery(ReviaSession& session, std::chrono::milliseconds emotionInterval,
        std::chrono::milliseconds relationshipQuiet, std::chrono::milliseconds conversationQuiet)
    {
        session.emotionSettleInterval = emotionInterval;
        session.relationshipQuietInterval = relationshipQuiet;
        session.quietConversationInterval = conversationQuiet;
    }

    static goals::Goal RunGoal(ReviaSession& session, goals::Goal goal, bool cancelBeforeRun = false)
    {
        std::lock_guard lock(session.operationMutex);
        (void)session.BeginOperation();
        if (cancelBeforeRun) session.RequestStop();
        return session.RunGoalUnlocked(std::move(goal));
    }

    static std::filesystem::path ActiveVoiceDirectory(const ReviaSession& session)
    { return session.speechService.ActiveVocalizationDirectory(); }

    static agents::MemoryAgent& Memory(ReviaSession& session)
    { return session.turnCoordinator.Memory(); }

    static void SubmitMemoryEvaluation(ReviaSession& session, std::string input, std::uint64_t turnId)
    { session.turnCoordinator.Memory().Submit(session.router, std::move(input), "",
        agents::ResponseProvenance::NormalGeneration, turnId); }

    static agents::LearnedFindingResult SubmitLearning(ReviaSession& session, memoryDecision decision)
    { return session.turnCoordinator.SubmitLearnedFinding(session.router, std::move(decision)); }

    static agents::LearnedFindingResult SubmitLearning(ReviaSession& session, const messageRouter& router, memoryDecision decision)
    {
        return session.turnCoordinator.SubmitLearnedFinding(router, std::move(decision), 47);
    }

    static void IdentitySaveEvery(ReviaSession& session, std::chrono::milliseconds interval)
    {
        session.identitySaveInterval = interval;
    }

    static void RememberIdentity(ReviaSession& session, const std::string& name)
    {
        session.relationships.SetDisplayName(identity::LocalUserEntityId(), name);
        session.relationships.ReinforcePreference("fixture astronomy", true,
            identity::PreferenceSource::Observed);
        auto development = session.relationships.Development();
        development.delta[identity::Trait::Patience] = 0.07F;
        session.relationships.SetDevelopment(development);
        auto mood = session.emotionRuntime.Mood();
        mood.valence = 0.31F;
        session.emotionRuntime.SetMood(mood);
    }

    static std::unique_lock<std::mutex> HoldForeground(ReviaSession& session)
    {
        return std::unique_lock(session.operationMutex);
    }

    static std::stop_token OperationToken(ReviaSession& session)
    {
        return session.CurrentOperationToken();
    }

    static void PrepareActions(ReviaSession& session, const std::filesystem::path& root)
    {
        std::string error;
        const bool initialized = session.actionRuntime.Initialize(
            root / "capabilities.json", root / "session-audit.jsonl", error);
        tests::Check(initialized, error);
        (void)session.BeginOperation();
    }

    static SessionResult Execute(ReviaSession& session, actions::ActionRequest request)
    {
        // Direct action fixtures simulate admission without starting unrelated session services.
        const bool wasStarted = session.started.exchange(true);
        try
        {
            auto result = session.ExecuteAction(std::move(request));
            session.started.store(wasStarted);
            return result;
        }
        catch (...)
        {
            session.started.store(wasStarted);
            throw;
        }
    }

    static void PrepareOperator(ReviaSession& session, const std::filesystem::path& root, goals::GoalRunner::StepProvider provider = {})
    {
        PrepareActions(session, root);
        session.goalStore = goals::GoalStore((root / "goals.db").string());
        if (provider) session.goalRunner.SetStepProvider(std::move(provider));
    }

    static void CheckOperatorAdmission(ReviaSession& session)
    {
        session.started.store(true);
        {
            ReviaSession::GoalTokenScope scope(session, {});
            tests::Check(scope.admitted && session.OperatorAdmitted({}),
                "Registering a goal invalidated its own captured policy revision.");
            session.companionAuthority->SetCompanionDefaults(session.Paths().Descriptor().id,
                policy::AuthorityPermissions::WithinMachineCeiling());
            tests::Check(!session.OperatorAdmitted({}), "An external authority change left operator admission current.");
        }
        session.started.store(false);
    }

    static void ConfigureLiveOperatorPlanner(ReviaSession& session, const int port)
    {
        tests::Check(session.config.LoadSettings(session.settings), "Live planner settings did not load.");
        session.settings.llm.port = port;
        session.router.ApplyLLMSettings(session.settings.llm, embeddingSettings{}, aiProfile{});
    }

    // The whole `/operate` path, with the finished goal handed back.
    //
    // Goes through Submit, so the request is parsed, the content is extracted before any
    // model call and the task boundary is opened and closed exactly as it is in use.
    // The goal itself comes back out of the store rather than being threaded through the
    // result, because the store is where the run actually recorded it -- and a harness
    // reading the run's own record is reading the same thing an audit would.
    static goals::Goal OperateRequest(ReviaSession& session, const std::string& request)
    {
        static_cast<void>(SubmitOperator(session, "/operate " + request));
        const std::vector<goals::Goal> recent = session.goalStore.LoadRecent(1);
        return recent.empty() ? goals::Goal{} : recent.front();
    }

    // The same task, stopped before any step runs.
    //
    // Deliberately not `Submit` with a stop requested first: Submit begins a new
    // operation, and beginning one installs a fresh stop source -- so a stop requested
    // beforehand is erased by the very call it was meant to interrupt. The first version
    // of this helper did exactly that and reported a fully executed run as a cancelled
    // one, which the generalization matrix then caught by asserting on the action count.
    // That is the right order for a mistake like this to be found in, and it is worth
    // leaving the reason written down.
    //
    // This goes through the same OperateGoal the other cancellation tests use, which
    // requests the stop *after* the operation exists, and opens the task boundary with
    // the content the request carried so the run is otherwise identical.
    static goals::Goal OperateRequestCancelled(ReviaSession& session, const std::string& request)
    {
        goals::Goal goal;
        goal.id = goals::NewGoalId();
        goal.title = request;
        goal.scope = goals::NarrowScopeForGoal(session.Capabilities());
        BeginComputerTask(session, goal.id, computer::ExtractTaskContent(request));
        goals::Goal finished =
            OperateGoal(session, std::move(goal), /*cancelBeforeRun=*/true);
        EndComputerTask(session);
        return finished;
    }

    // Tasks run in the background. This waits for one the input started and reports
    // its outcome, which is what the operator tests assert on.
    static SessionResult SubmitOperator(ReviaSession& session, const std::string& input)
    {
        // Exercise the real input/lock owner without starting model or sensor workers.
        session.started.store(true);
        try
        {
            const std::uint64_t launchedBefore = session.tasksLaunched.load();
            auto result = session.Submit(input);
            if (session.tasksLaunched.load() != launchedBefore)
            {
                WaitForTask(session);
                std::lock_guard lock(session.taskMutex);
                if (session.lastTaskReport)
                {
                    result.succeeded = session.lastTaskReport->status == goals::GoalStatus::Succeeded;
                    result.text = session.lastTaskReport->summary;
                }
            }
            session.started.store(false);
            return result;
        }
        catch (...)
        {
            session.started.store(false);
            throw;
        }
    }

    // The computer-task owner, for the integration tests that drive the real decision
    // path. Reached rather than reconstructed: a test that built its own coordinator
    // would be testing a copy of the wiring instead of the wiring.
    static computer::ComputerTaskCoordinator& ComputerTasks(ReviaSession& session)
    { return session.computerTasks; }

    // Replace the decision providers with scripted ones, so the whole path from a
    // request through a bounded subgoal to an executed step runs with no model and no
    // network.
    // The scripted form takes a one-string subgoal answerer for convenience: a test
    // that supplies the answer has no use for the instruction or the schema, and
    // threading them through every scripted lambda would obscure what each test is
    // actually saying.
    static void ScriptComputerProviders(ReviaSession& session,
        std::function<responseOutput(const std::string&, std::stop_token)> subgoalPlanner,
        std::function<responseOutput(const std::string&, std::stop_token)> stepPlanner)
    {
        session.computerTasks.SetSubgoalPlanner(
            [subgoalPlanner = std::move(subgoalPlanner)](
                const std::string&, const std::string& situation, const std::string&,
                std::stop_token stopToken)
            {
                return subgoalPlanner(situation, std::move(stopToken));
            });
        session.computerTasks.SetLegacyPolicy(
            std::make_unique<computer::LegacyLlmComputerPolicy>(std::move(stepPlanner)));
    }

    // What one call to a model cost, recorded as it happens.
    //
    // Deliberately a pass-through and not a script. The prompt that goes out is the one
    // the runtime built and the answer that comes back is the model's -- this only
    // watches. A test that returned canned answers here would be measuring its own
    // fixture, which is exactly the thing a live acceptance run exists to stop doing.
    struct ModelCall
    {
        std::string kind;
        std::size_t promptBytes = 0;
        std::size_t responseBytes = 0;
        std::uint32_t tokens = 0;
        bool tokensReported = false;
        bool succeeded = false;
        long long milliseconds = 0;
        std::string response;
    };

    // Route both decision calls through the real router, recording each one.
    static void InstrumentComputerProviders(ReviaSession& session, std::vector<ModelCall>& log)
    {
        const auto record = [&log](
            std::string kind,
            const std::size_t promptBytes,
            const std::chrono::steady_clock::time_point started,
            const responseOutput& answer)
        {
            ModelCall call;
            call.kind = std::move(kind);
            call.promptBytes = promptBytes;
            call.responseBytes = answer.response.size();
            call.tokens = answer.TotalTokens();
            call.tokensReported = answer.bTokensReported;
            call.succeeded = answer.bSuccess;
            call.milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            call.response = answer.response;
            log.push_back(std::move(call));
        };

        // Each through its own route, because they are different questions under
        // different grammars. Wrapping both in PlanNextGoalStep is precisely the
        // mistake this instrumentation exists to catch.
        session.computerTasks.SetSubgoalPlanner(
            [&session, record](const std::string& instruction,
                const std::string& situation, const std::string& schema,
                std::stop_token stopToken)
            {
                const auto started = std::chrono::steady_clock::now();
                const responseOutput answer = session.router.PlanComputerSubgoal(
                    instruction, situation, schema, std::move(stopToken));
                record("subgoal", instruction.size() + situation.size(), started, answer);
                return answer;
            });
        session.computerTasks.SetLegacyPolicy(
            std::make_unique<computer::LegacyLlmComputerPolicy>(
                [&session, record](const std::string& prompt, std::stop_token stopToken)
                {
                    const auto started = std::chrono::steady_clock::now();
                    const responseOutput answer =
                        session.router.PlanNextGoalStep(prompt, std::move(stopToken));
                    record("next_step", prompt.size(), started, answer);
                    return answer;
                }));
    }

    static void SetComputerSettings(ReviaSession& session, const computerControlSettings& settings)
    {
        session.settings.computerControl = settings;
        session.computerTasks.ApplySettings(settings);
    }

    // The real step provider the session installs, rather than a stand-in. This is what
    // makes an integration test an integration test: the provider under test is the one
    // the constructor wired, complete with the ordinal, requested-by and visual target
    // stamping that happens around it.
    static void UseRealStepProvider(ReviaSession& session)
    {
        session.goalRunner.SetStepProvider(
            [&session](const goals::Goal& goal, const std::uint32_t iteration)
            {
                goals::NextStep next = session.computerTasks.Decide(
                    goal, iteration, session.settings.perception,
                    session.CurrentOperationToken());
                if (!next.hasStep) return next;
                next.step.ordinal = static_cast<std::uint32_t>(goal.steps.size());
                next.step.action.requestedBy = "goal";
                next.step.check.requestedBy = "goal";
                return next;
            });
    }

    // The iterative runner, driven through the session that owns it.
    //
    // Deliberately Operate and not Run: the planned runner reads a fixed vector and
    // never asks a provider anything, so a test that used it would exercise none of the
    // decision path. The approval prompt and the command parsing above it have their own
    // tests; what this reaches is the runner, the provider the constructor installed,
    // and everything the provider calls.
    static goals::Goal OperateGoal(ReviaSession& session, goals::Goal goal, const bool cancelBeforeRun = false)
    {
        std::lock_guard lock(session.operationMutex);
        struct RestoreStarted
        {
            std::atomic<bool>& value;
            bool previous;
            ~RestoreStarted() { value.store(previous); }
        } restoreStarted{session.started, session.started.exchange(true)};
        (void)session.BeginOperation();
        if (cancelBeforeRun) session.RequestStop();
        const auto stopToken = session.CurrentOperationToken();
        const ReviaSession::GoalTokenScope scope(session, stopToken);
        tests::Check(scope.admitted, "The operator fixture could not register its real goal authority.");
        tests::Check(cancelBeforeRun || session.OperatorAdmitted(stopToken),
            "The operator fixture did not retain current observation admission.");
        // Exactly what the /operate path does once the person approves the task: a
        // standing yes bounded to this run, and the desktop task approval that raises
        // the consequence ceiling for it. Without both, an ordinary content edit is
        // refused -- correctly -- and the run would be demonstrating the refusal rather
        // than the path.
        session.goalRunner.SeedStandingApproval(actions::RiskLevel::ReversibleWrite, true);
        const auto approval = session.actionRuntime.ApproveDesktopTask(goal.id, false);
        static_cast<void>(approval);
        return session.goalRunner.Operate(
            std::move(goal), stopToken);
    }

    static void BeginComputerTask(ReviaSession& session, const std::string& goalId, computer::TaskContent content = {})
    {
        session.computerTasks.BeginTask(
            goalId, computer::RequestOrigin::UserDirected, std::move(content));
    }

    static void EndComputerTask(ReviaSession& session)
    { session.computerTasks.EndTask(); }

    static void UseDatasetRoot(ReviaSession& session, const std::filesystem::path& datasetRoot)
    {
        tests::Check(session.computerTasks.Recorder().SetRoot(datasetRoot),
            "The dataset root could not be set; a capture session is already open.");
    }

    static void CompleteComputerTask(ReviaSession& session, const goals::Goal& finished)
    { session.computerTasks.CompleteTask(finished); }

    static void ObserveActions(ReviaSession& session, actions::ActionRuntime::DispatchObserver observer)
    {
        session.actionRuntime.SetDispatchObserver(std::move(observer));
    }
};

} // namespace revia::runtime
