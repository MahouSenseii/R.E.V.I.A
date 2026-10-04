#pragma once

#include "Core/exitReporter.h"
#include "Core/questionRelay.h"
#include "Learning/qualityFeedback.h"
#include "Runtime/reviaSession.h"

#include <QMainWindow>
#include <QPointer>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

class QCheckBox;
class QCloseEvent;
class QResizeEvent;
class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
class QMessageBox;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QSystemTrayIcon;
class QTabWidget;
class QTextBrowser;
class QTimer;
class QToolButton;
class QWidget;
class CanvasPanel;
class InternetActivityPanel;
class PipelinePanel;
class ResourcePanel;
class CapabilityPanel;
class ProfilePanel;
class MemoryPanel;
class MindPanel;
class VisionPanel;
class VoiceHealthPanel;
class AgentStudioPanel;
class LearningStudioPanel;
class DevelopmentStudioPanel;
class AudienceStudioPanel;
class ConversationStatusPanel;
class AnswerFeedbackDialog;
namespace Ui
{
class ReviaWindow;
}

class ReviaWindow final : public QMainWindow
{
  public:
    // Authoritative microphone presentation state. Phase events from the recognition
    // service drive it; the toggle only ever requests a transition.
    enum class MicrophoneUi
    {
        Unavailable,
        Ready,
        HandsFree,
        Listening,
        Transcribing
    };

    explicit ReviaWindow(bool startRuntime = true, bool buildSystemTray = true, QWidget* parent = nullptr);
    ~ReviaWindow() override;
    void RequestShutdown(revia::core::ExitReason reason = revia::core::ExitReason::SmokeTest);
    bool IsRuntimeStarted() const;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    friend struct ReviaWindowStopTests;
    void BuildInterface();
    revia::runtime::ReviaSession& Session();
    const revia::runtime::ReviaSession& Session() const;
    void BindSession();
    void BuildCompanionControls();
    void BuildAdditionalStudioPanels();
    void RefreshStudioPanels();
    void RunStudioOperation(std::function<bool(revia::runtime::ReviaSession&, std::stop_token, std::string&)> operation,
        std::function<void(bool, const std::string&)> completed);
    void RefreshCompanions();
    void SwitchCompanion(const std::string& id);
    void CreateCompanion();
    void RebuildSessionPanels();
    // Keep a readable measure while allowing wide windows to use their extra space.
    void ApplyContentWidthCap();
    void ApplyResponsiveLayout();
    void BuildTray();
    void StartRuntime();
    void SendMessage(bool voiceInput = false);
    void ToggleListening();
    // Rebuilds the device list from what Windows reports and restores the saved
    // selection. Called at startup and from Refresh.
    void RefreshMicrophoneDevices();
    void RunMicrophoneTest();
    void ApplyMicrophoneUi(MicrophoneUi microphoneUi);
    void UseVisibleScreen();
    void DiscoverApplicationPermissions();
    // The voice studio creates and previews voices; which profile speaks with which one
    // is decided in ProfilePanel, so refreshing here refreshes that too.
    void RefreshVoiceStudio();
    void CreateVoicePreset();
    void RenderVoiceBank();
    void PreviewVoice();
    // Every shutdown names its own cause, so the ledger never has to guess between a
    // deliberate quit and something that merely looked like one.
    void BeginShutdown(revia::core::ExitReason reason, const std::string& detail = {});
    void ToggleMaximized();
    void UpdateMaximizeButton();
    void SetAlwaysOnTop(bool enabled);
    void HandleRuntimeEvent(const revia::runtime::RuntimeEvent& event);
    void PresentSessionResult(const revia::runtime::SessionResult& result);
    void CaptureDisplayedAnswer(const QString& text, const revia::runtime::RuntimeStamp& origin, std::uint64_t audienceRevision,
        std::uint64_t turnId = 0, bool fragment = false);
    void ReviewDisplayedAnswer();
    void ResetConversationPresentation();
    void ConfigureMemoryRevisionControls();
    void UpdateState(revia::runtime::RuntimeState state, const QString& detail);
    // The runtime returns to Idle as soon as a turn finishes, but speech is generated and
    // played afterwards on its own worker. Showing Idle while Revia is audibly about to
    // talk is wrong, so the badge reflects whichever of the two is actually busy.
    void RefreshStateBadge();
    revia::runtime::RuntimeState lastRuntimeState = revia::runtime::RuntimeState::Offline;
    QString lastRuntimeDetail;
    QString speechPhase;
    // reasoning, when present, is rendered as a collapsed "Thought process" line that the
    // user can expand. It is kept out of the message body because it is not an answer.
    void AppendChat(const QString& speaker, const QString& message, bool userMessage = false, const QString& reasoning = QString());
    // Transcript entry kinds drive rendering independently of speaker names.
    // QTextBrowser collapses sections by rebuilding the transcript when a link is clicked.
    enum class EntryKind
    {
        Message,
        SelfInquiry,
        InvestigationChecking,
        InvestigationFindings
    };

    struct ChatEntry
    {
        QString speaker;
        QString body;
        QString reasoning;
        bool userMessage = false;
        bool expanded = false;
        EntryKind kind = EntryKind::Message;
        // The task and round this belongs to, so a round from a superseded question can
        // never be drawn under a later one.
        quint64 taskId = 0;
        int round = 0;
    };

    // Appends one of Revia's working entries. `detail` is the supporting evidence, shown
    // only when the user expands it.
    void AppendWorkEntry(EntryKind kind, const QString& body, const QString& detail, quint64 taskId, int round);
    // Display only. Turning this off hides the blocks and changes nothing about whether
    // the investigation runs -- enablement lives in the runtime settings, not here.
    bool showWorkSummaries = true;
    void RenderChat();
    std::vector<ChatEntry> chatEntries;
    enum class ActivitySeverity
    {
        Automatic,
        Information,
        Warning,
        Error
    };
    struct ActivityEntry
    {
        QString message;
        ActivitySeverity severity = ActivitySeverity::Information;
    };
    void AppendActivity(const QString& message, ActivitySeverity severity = ActivitySeverity::Automatic);
    void AppendComponentActivity(const revia::runtime::RuntimeEvent& event, const QString& message);
    void RenderActivity();
    void UpdateActivitySummary();
    void ApplyUserPreferences();
    void RefreshPresenceUi();
    void ShowPreferenceResult(const revia::core::PreferenceResult& result);
    std::function<bool()> CaptureQuestionAdmission();
    bool RunAdmittedQuestion(QMessageBox& question, const std::function<bool()>& admitted);
    // The specific yes a control like Send needs. Separate from ConfirmAction: that
    // one confirms a typed action before policy runs, this one answers an
    // authorization that stopped on the consequence of a control.
    bool ApproveDesktopEffect(const revia::policy::ApprovalPrompt& prompt);
    revia::actions::ConfirmationChoice ConfirmAction(
        const revia::actions::ActionRequest& request, const revia::actions::PolicyDecision& decision);
    revia::core::QuestionRelay::Post PostToWindow();
    // Refuses every pending approval and closes the one on screen. UI thread only.
    void AbandonQuestions();
    static QIcon CreateReviaIcon();

    // Before the session, so it outlives every worker that could ask a question.
    revia::core::QuestionRelay questions;
    QPointer<QMessageBox> openQuestion;
    revia::runtime::ReviaSession session;
    std::unique_ptr<revia::runtime::ReviaSession> selectedSession;
    std::unique_ptr<revia::runtime::CompanionRegistry> companions;
    std::set<std::string> initializedCompanionAuthority{"legacy"};
    std::atomic<std::uint64_t> sessionUiEpoch{1};
    std::jthread companionSwitchWorker;
    bool switchingCompanion = false;
    QComboBox* companionCombo = nullptr;
    QLabel* companionStatus = nullptr;
    QPushButton* createCompanionButton = nullptr;
    AgentStudioPanel* agentStudioPanel = nullptr;
    LearningStudioPanel* learningStudioPanel = nullptr;
    DevelopmentStudioPanel* developmentStudioPanel = nullptr;
    AudienceStudioPanel* audienceStudioPanel = nullptr;
    std::jthread studioWorker;
    std::atomic<bool> studioOperationRunning = false;
    revia::runtime::RuntimeEventBus::SubscriptionId subscriptionId = 0;
    std::unique_ptr<Ui::ReviaWindow> ui;

    QLabel* stateLabel = nullptr;
    QLabel* stateDetailLabel = nullptr;
    QLabel* affectLabel = nullptr;
    QLabel* speechLabel = nullptr;
    QLabel* microphoneLabel = nullptr;
    QLabel* automationLabel = nullptr;
    QLabel* visionLabel = nullptr;
    QLabel* perceptionLabel = nullptr;
    QWidget* titleBar = nullptr;
    QToolButton* maximizeButton = nullptr;
    QTextBrowser* chatHistory = nullptr;
    QPlainTextEdit* messageInput = nullptr;
    QTextBrowser* activityFeed = nullptr;
    QLabel* activityIssueSummary = nullptr;
    QComboBox* activityFilter = nullptr;
    QCheckBox* activityAutoScroll = nullptr;
    QPushButton* openLogsButton = nullptr;
    QPushButton* clearActivityButton = nullptr;
    std::vector<ActivityEntry> activityEntries;
    std::map<QString, std::pair<QString, qint64>> lastComponentIssues;
    int activityWarningCount = 0;
    int activityErrorCount = 0;
    PipelinePanel* pipelinePanel = nullptr;
    InternetActivityPanel* internetActivityPanel = nullptr;
    ResourcePanel* resourcePanel = nullptr;
    CanvasPanel* canvasPanel = nullptr;
    CapabilityPanel* capabilityPanel = nullptr;
    ProfilePanel* profilePanel = nullptr;
    MemoryPanel* memoryPanel = nullptr;
    MindPanel* mindPanel = nullptr;
    VisionPanel* visionPanel = nullptr;
    VoiceHealthPanel* chatVoiceHealth = nullptr;
    VoiceHealthPanel* voiceHealth = nullptr;
    ConversationStatusPanel* conversationStatus = nullptr;
    QPointer<AnswerFeedbackDialog> answerFeedbackDialog;
    QString latestDisplayedAnswer;
    revia::learning::QualityFeedback displayedAnswerTarget;
    std::uint64_t displayedAnswerTurnId = 0;
    QTabWidget* tabs = nullptr;
    QPushButton* sendButton = nullptr;
    QPushButton* stopButton = nullptr;
    QPushButton* microphoneButton = nullptr;
    QPushButton* screenActionButton = nullptr;
    QComboBox* microphoneDeviceCombo = nullptr;
    QPushButton* refreshMicrophonesButton = nullptr;
    QPushButton* testMicrophoneButton = nullptr;
    QLabel* microphoneTestResultLabel = nullptr;
    QCheckBox* alwaysOnTopCheck = nullptr;
    QCheckBox* speechCheck = nullptr;
    QCheckBox* autoSendVoiceCheck = nullptr;
    QCheckBox* bargeInCheck = nullptr;
    QCheckBox* handsFreeCheck = nullptr;
    QCheckBox* avatarBridgeCheck = nullptr;
    QCheckBox* externalAdaptersCheck = nullptr;
    QCheckBox* initiativeCheck = nullptr;
    QCheckBox* curiosityCheck = nullptr;
    QCheckBox* spontaneousSpeechCheck = nullptr;
    QCheckBox* speakWhenAwayCheck = nullptr;
    QCheckBox* aiFilterCheck = nullptr;
    QSpinBox* initiativeMaxSpin = nullptr;
    QSpinBox* resourceSampleSpin = nullptr;
    QLabel* preferenceStatus = nullptr;
    QLabel* presencePhaseValue = nullptr;
    QLabel* presenceAffectValue = nullptr;
    QLabel* presenceAttentionValue = nullptr;
    QProgressBar* presenceMomentumBar = nullptr;
    QPushButton* openPresenceFolderButton = nullptr;
    QComboBox* voiceLibraryCombo = nullptr;
    QComboBox* voiceLanguageCombo = nullptr;
    QLineEdit* voiceNameInput = nullptr;
    QPlainTextEdit* voiceDescriptionInput = nullptr;
    QPlainTextEdit* voiceReferenceInput = nullptr;
    QPlainTextEdit* voicePreviewInput = nullptr;
    QLabel* voiceStudioStatus = nullptr;
    QPushButton* createVoiceButton = nullptr;
    QPushButton* previewVoiceButton = nullptr;
    QPushButton* renderVoiceBankButton = nullptr;
    QSystemTrayIcon* trayIcon = nullptr;
    QTimer* pollTimer = nullptr;

    std::jthread operationWorker;
    std::jthread shutdownWorker;
    std::jthread voiceWorker;
    std::jthread capabilityWorker;
    // Device probes hold a microphone open for seconds. Separate from voiceWorker so
    // a running test cannot be joined by an unrelated voice operation.
    std::jthread deviceWorker;
    std::atomic<bool> shuttingDown = false;
    std::atomic<bool> voiceOperationRunning = false;
    // A reply that has been generated but is waiting for its audio to start, so the words
    // land with the voice. Released early on any speech failure, and by a timer, because
    // text that is never shown is a worse outcome than text shown slightly ahead.
    // Keyed by utterance because a streamed reply produces several, each with its own
    // audio. Speak() only queues; with Qwen the audio starts seconds later, so showing
    // text at queue time is not synchronisation at all. Pass 0 to release everything.
    void ReleasePendingSpeechText(std::uint64_t utteranceId);
    struct PendingUtterance
    {
        QString speaker;
        QString text;
        QString reasoning;
    };
    std::map<std::uint64_t, PendingUtterance> pendingUtterances;
    // Speech starts before Submit returns, so the Speaking event can arrive before the
    // shell has anything stored to release. Remembering the last one seen means the text
    // is shown at once rather than waiting out the fallback timer.
    std::uint64_t lastSpeakingUtteranceId = 0;
    QTimer* pendingSpeechTimer = nullptr;

    bool speechActive = false;
    bool microphoneActive = false;
    bool listenRequested = false;
    MicrophoneUi microphoneUiState = MicrophoneUi::Unavailable;
};
