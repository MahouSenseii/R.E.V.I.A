#pragma once

#include "Core/exitReporter.h"
#include "attachmentTray.h"
#include "Core/questionRelay.h"
#include "Learning/qualityFeedback.h"
#include "Runtime/reviaSession.h"

#include <QMainWindow>
#include <QPointer>
#include <QStringList>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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
class ChatSessionsPanel;
class NavigationSidebar;
class DocumentsDialog;
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
    void BuildNavigationSidebar();
    void BuildTray();
    void StartRuntime();
    // Takes operationWorker for a new foreground operation. Refuses, with a status line,
    // while the previous one is still running, and only ever joins a finished worker,
    // because joining a running one on this thread is what froze the window.
    bool ClaimOperationWorker();
    void SendMessage(bool voiceInput = false);
    // Attachments: the button, drag and drop, and pasted pictures all end here. Files are
    // read on a worker thread and staged in the session; the tray shows the result.
    void BuildAttachmentControls();
    void ChooseAttachments();
    void AttachPaths(const QStringList& paths);
    void AttachPastedImage(const QImage& image);
    void RemoveAttachmentChip(const std::string& id);
    // Accepts a drag of local files or a picture onto the chat; true when it handled the event.
    bool HandleAttachmentDrag(QEvent* event);
    void ShowDocuments();
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
    // The chat list beside the transcript, backed by the conversation archive.
    void BuildChatSidebar();
    void ApplyChatSidebarVisibility();
    void RefreshChatSidebar();
    bool StartNewChat(std::string& outError);
    bool DeleteChat(const std::string& id, std::string& outError);
    // A past chat is shown read-only in place of the live transcript, which keeps
    // updating underneath; Back returns to it and Continue makes the past chat current.
    void OpenArchivedChat(const std::string& id);
    void CloseArchivedChat();
    void ContinueArchivedChat();
    void ClearTranscript();
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
    // Shows the reply that was approved whole in place of the fragments streamed for the
    // same turn, when the final filter pass changed text that had already appeared.
    void ReplaceStreamedReply(std::uint64_t conversationTurnId, const QString& speaker, const QString& body);
    // Transcript entry kinds drive rendering independently of speaker names.
    // QTextBrowser collapses sections by rebuilding the transcript when a link is clicked.
    enum class EntryKind
    {
        Message,
        SelfInquiry,
        InvestigationChecking,
        InvestigationFindings,
        // A task Revia handed to her agents from chat, shown under the reply that did it.
        DelegatedTask
    };

    // What a delegated-task card follows and last showed. The text is kept, not recomputed,
    // once the session reports on another task or none, so an old card still says honestly
    // how its own task ended.
    struct DelegatedCard
    {
        std::string workflowId;
        std::uint64_t originTurnId = 0;
        QString objective;
        // The last phase the session published for this task, used once it no longer
        // reports on the task directly.
        std::optional<revia::runtime::DelegatedTaskStatus> published;
        QString publishedDetail;
        QString status;
        QString progress;
        // Running, as far as the session last said: the only state that offers Cancel.
        bool active = true;
        // What the shown text was computed from. A poll that changes none of it renders nothing.
        QString basis;
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
        // Revia's own replies are Markdown; everything else is shown as plain text.
        bool markdown = false;
        // The event phase that last wrote a working entry, so streamed thinking can tell
        // a continuation from a new stage of the same turn.
        QString phase;
        // The body as last rendered and the text it came from. A re-render only converts
        // entries whose text changed, so a streamed sentence does not re-run Markdown
        // over the whole history.
        QString renderedSource;
        QString renderedBody;
        // The files sent with a user message, shown under it.
        std::vector<AttachmentChip> attachments;
        // Set for EntryKind::DelegatedTask only.
        DelegatedCard delegated;
    };

    // Appends one of Revia's working entries. `detail` is the supporting evidence, shown
    // only when the user expands it.
    void AppendWorkEntry(EntryKind kind, const QString& body, const QString& detail, quint64 taskId, int round);
    // Chat delegation. Agent Studio and every task card are refreshed together from one read
    // of the workflow and the delegated task, so the two views can never disagree.
    // `transcriptChanged` renders the chat even when no card's text changed.
    void RefreshAgentWorkflowViews(bool transcriptChanged = false);
    // A Delegation event: starts a card on submission and records each later phase.
    // Returns whether it added an entry to the transcript.
    bool ObserveDelegation(const revia::runtime::RuntimeEvent& event);
    // True when the card's shown text changed.
    bool UpdateTaskCard(ChatEntry& entry, const revia::agents::WorkflowSnapshot& snapshot,
        const std::optional<revia::runtime::DelegatedTaskView>& task);
    // Appends the held card, unless a card for its task is already on screen. The caller
    // trims and renders; true when it appended.
    bool PlaceHeldTaskCard();
    // By task id, never by a stored index: trimming shifts every index.
    [[nodiscard]] std::optional<std::size_t> FindTaskCard(const std::string& workflowId) const;
    // The submission arrives mid-turn, before the reply it belongs under, so its card waits
    // here until that reply is on screen. One workflow owner means one card at most.
    std::optional<ChatEntry> heldTaskCard;
    // The latest conversation turn seen to end, so a submission arriving after its own turn
    // is placed at once instead of waiting for a reply that has already been shown.
    std::uint64_t lastFinishedConversationTurn = 0;
    // Display only. Turning this off hides the blocks and changes nothing about whether
    // the investigation runs -- enablement lives in the runtime settings, not here.
    bool showWorkSummaries = true;
    // `keepReaderPosition` leaves a reader who scrolled up where they were; otherwise the
    // transcript follows its newest line, as it always has.
    void RenderChat(bool keepReaderPosition = false);
    // The cached body HTML of one entry, converted again only when its text changed.
    const QString& RenderedBody(ChatEntry& entry);
    // Consecutive entries from one non-user speaker read as one message: streamed speech
    // arrives sentence by sentence. These find where such a message ends and its text.
    [[nodiscard]] bool EndsMessage(std::size_t index) const;
    [[nodiscard]] QString MessageText(std::size_t last) const;
    // The transcript keeps the newest entries only. Indices held elsewhere (streamed
    // fragments) are shifted, or forgotten when their entry scrolled away.
    static constexpr std::size_t MaximumChatEntries = 400;
    void TrimTranscript();
    // How many entries trimming has dropped from the front, so a position kept across
    // an asynchronous operation can still be found afterwards.
    std::size_t chatEntriesDropped = 0;
    // Regenerate and edit-and-resend act on the latest exchange only: the last user
    // line, and the reply from Revia after it.
    [[nodiscard]] std::optional<std::size_t> LastUserEntry() const;
    [[nodiscard]] std::optional<std::size_t> RegenerableReply() const;
    // Runs ReviaSession::RegenerateLast, or ReplaceLastUserTurn when `replacement` is
    // given. The transcript changes at once and is put back if the session refuses.
    void RetryLastExchange(const QString& replacement);
    void BeginEditLastTurn();
    void CancelEditLastTurn();
    bool editingLastTurn = false;
    // A foreground turn has been sent and its result is not shown yet; the regenerate
    // and edit links stay hidden until it is.
    bool chatTurnInFlight = false;
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
    ChatSessionsPanel* chatSessionsPanel = nullptr;
    QPushButton* chatSidebarToggle = nullptr;
    QWidget* archivedChatBanner = nullptr;
    QLabel* archivedChatLabel = nullptr;
    QPushButton* continueChatButton = nullptr;
    std::string viewingArchivedChat;
    bool chatSidebarWanted = true;
    QPointer<AnswerFeedbackDialog> answerFeedbackDialog;
    QString latestDisplayedAnswer;
    revia::learning::QualityFeedback displayedAnswerTarget;
    std::uint64_t displayedAnswerTurnId = 0;
    // The transcript entries that one turn's streamed fragments occupy, in order.
    std::uint64_t streamedReplyTurnId = 0;
    std::vector<std::size_t> streamedReplyEntries;
    QTabWidget* tabs = nullptr;
    NavigationSidebar* navigationSidebar = nullptr;
    QPushButton* runtimeDetailsButton = nullptr;
    QPushButton* sendButton = nullptr;
    QPushButton* attachButton = nullptr;
    QPushButton* documentsButton = nullptr;
    AttachmentTray* attachmentTray = nullptr;
    QPointer<DocumentsDialog> documentsDialog;
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
    QComboBox* delegationModeCombo = nullptr;
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
    // Set by ClaimOperationWorker, cleared by the worker itself as its body ends.
    std::atomic<bool> operationRunning = false;
    std::jthread shutdownWorker;
    std::jthread voiceWorker;
    std::jthread capabilityWorker;
    // Device probes hold a microphone open for seconds. Separate from voiceWorker so
    // a running test cannot be joined by an unrelated voice operation.
    std::jthread deviceWorker;
    std::atomic<bool> shuttingDown = false;
    std::atomic<bool> voiceOperationRunning = false;
    // Reads attached files off the GUI thread; a large PDF takes a moment to extract.
    std::jthread attachmentWorker;
    std::atomic<bool> attachmentRunning = false;
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
