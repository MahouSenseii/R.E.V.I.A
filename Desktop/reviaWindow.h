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

class QAction;
class QCheckBox;
class QCloseEvent;
class QResizeEvent;
class QShowEvent;
class QComboBox;
class QEvent;
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QMessageBox;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QSystemTrayIcon;
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
class DocumentsDialog;
class AnswerFeedbackDialog;
namespace Ui
{
class ReviaWindow;
}
namespace revia::desktop
{
class CommandPalette;
class ComposerCard;
class PanelHost;
class SerialWorker;
class SettingsDialog;
class ShellSidebar;
class ShortcutSheet;
class ToastOverlay;
enum class PanelId;
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
    // Settings asked for a restart; main relaunches the app once this window is gone.
    [[nodiscard]] bool RestartRequested() const
    {
        return restartRequested;
    }
    // --ui-screenshot-tab: "chat", a panel key or title prefix, then a Settings section; "settings"
    // or "settings:<section>" for Settings itself. Returns the window that now shows it, or null.
    QWidget* ShowScreenshotTarget(const QString& name);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

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
    // Window-size rules (design spec 2.11): the sidebar falls back to its rail below 900px and the
    // header drops its detail text below 640px of chat column.
    void ApplyResponsiveLayout();
    // Elides the state's detail to the room the chat header has left (at most 280px), and hides it
    // when there is too little, so the header never asks for more width than the window has.
    void FitStateDetail();
    // Settings' reflow at its content width: the permission cards' columns and the microphone row.
    void ApplySettingsLayout(int contentWidth);
    // The Activity window's toolbar: two columns below 640px, otherwise one row.
    void ApplyActivityLayout(int width);
    // The secondary pages become panel windows; the Settings dialog gets the session backend and
    // the window's own rows; then the sidebar, chat column, menus, palette and shortcuts are built
    // around the chat page (windowShell.cpp).
    void BuildPanels();
    void BuildSettingsDialog();
    void BuildShell();
    void BuildChatColumn(QWidget* parent);
    void BuildComposer();
    void BuildMenus();
    void BuildPalette();
    void BuildShortcuts();
    [[nodiscard]] std::vector<std::pair<QString, QString>> ShortcutRows() const;
    void RefreshProfileMenu();
    void RefreshSidebarMenu();
    void RefreshAccount();
    void RefreshChatTitle();
    // Disables everything the owner could use to start work while a companion switch or shutdown
    // runs, and says so over the chat. Shutdown never unlocks.
    void SetNavigationLocked(bool locked);
    // Every destination the shell can show: the chat page, each panel's page (Mind's four views
    // instead of Mind) and the Settings sections.
    [[nodiscard]] QList<QWidget*> NavigationPages() const;
    [[nodiscard]] QString NavigationTitle(QWidget* page) const;
    // A section name ("Voice"), a setting key or objectName, or empty for the last section shown.
    void OpenSettings(const QString& sectionOrKey = {});
    void OpenPanel(revia::desktop::PanelId id);
    void ShowChat();
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
    void SetAlwaysOnTop(bool enabled);
    // Settings' write hook for the rows the window itself acts on (window, sidebar, transcript).
    void ApplyShellSetting(const std::string& key, bool value);
    void ApplyChatTextSize(int pixels);
    // Clears the cached Markdown (it bakes colours in) and redraws everything themed by hand.
    void ApplyThemeToWindow();
    // The status line: shown as a toast and kept as statusBar()->currentMessage().
    void ShowStatus(const QString& text, int timeoutMs);
    // A long-lived mode under the composer instead of a toast; the status line keeps the same text.
    void ShowComposerContext(const QString& context, const QString& actionText, std::function<void()> action);
    // "{name} is still working on the previous request": the status line and the composer.
    void ShowStillWorking();
    enum class ComposerContextKind
    {
        None,
        Editing,
        Queued,
        StillWorking
    };
    ComposerContextKind composerContextKind = ComposerContextKind::None;
    // Settings › Models › Use the cloud for, read from the owner overlay.
    void RefreshCloudUseFor();
    void HandleRuntimeEvent(const revia::runtime::RuntimeEvent& event);
    void PresentSessionResult(const revia::runtime::SessionResult& result);
    void CaptureDisplayedAnswer(const QString& text, const revia::runtime::RuntimeStamp& origin, std::uint64_t audienceRevision,
        std::uint64_t turnId = 0, bool fragment = false);
    void ReviewDisplayedAnswer();
    void ResetConversationPresentation();
    // The chat list beside the transcript, backed by the conversation archive.
    void BuildChatSidebar();
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
    // Send is enabled only while a message could go now; Stop takes the send slot while a turn,
    // task or studio operation runs, and sits beside it while only speech or the microphone does.
    void UpdateSendState();
    void RefreshPrivacyIndicators();
    // The conversation strip shows while the runtime works, while its details are open or while it
    // carries a quality flag for the latest reply.
    void UpdateConversationStrip();
    void ToggleStatusPopover();
    // The header Stop, the sidebar and palette entries: halts desktop control and the running turn.
    void StopDesktopControlFromShell();
    // Puts the transcript in its 760px measure without re-rendering it.
    void ApplyChatMeasure();
    void ApplyComposerMeasure();
    [[nodiscard]] QString DisplayName() const;
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
        // An unprompted offer (a Proposal event): drawn as a quiet activity row.
        bool proposal = false;
    };

    // Appends one of Revia's working entries. `detail` is the supporting evidence, shown
    // only when the user expands it.
    void AppendWorkEntry(EntryKind kind, const QString& body, const QString& detail, quint64 taskId, int round);
    // Display only. Turning this off hides the blocks and changes nothing about whether
    // the investigation runs -- enablement lives in the runtime settings, not here.
    bool showWorkSummaries = true;
    void RenderChat();
    // The transcript's default stylesheet: the theme's classes at the owner's chat text size.
    [[nodiscard]] QString TranscriptCss() const;
    [[nodiscard]] QString UserBubbleHtml(const QString& text) const;
    // The greeting, starting or offline note an empty chat shows (design spec 2.7).
    [[nodiscard]] QString EmptyStateHtml() const;
    // The newest reply that is complete, which alone carries Copy, Regenerate and Review.
    [[nodiscard]] std::optional<std::size_t> LatestFinishedReply() const;
    void RenderArchivedChat(const std::vector<revia::memory::ArchivedTurn>& turns);
    // Set while RenderChat replaces the document, whose scroll changes are not the reader's.
    bool renderingTranscript = false;
    // A thought: toggle keeps the reader where they were.
    bool renderKeepsPosition = false;
    QString lastTranscriptHtml;
    revia::runtime::RuntimeState lastRenderedState = revia::runtime::RuntimeState::Offline;
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
    // A reply that could not be finished is drawn as one row after the entry it followed, without
    // becoming a chat entry. A stop the owner asked for reads "Stopped" instead.
    struct TurnError
    {
        QString reason;
        std::size_t afterEntry = 0;
        quint64 taskId = 0;
        bool stoppedByUser = false;
    };
    std::optional<TurnError> lastTurnError;
    void RecordTurnError(const revia::runtime::SessionResult& result);
    // Set by Stop, Esc and the header Stop before they stop the turn; cleared by the next send.
    bool userStopRequested = false;
    void RetryFailedTurn();
    void CopyMessageAt(const QPoint& position);
    // The transcript stays at the bottom while it is followed there; scrolled up, it keeps its
    // place and offers Latest once something new arrives.
    bool transcriptPinned = true;
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
    [[nodiscard]] QString ActivityColour(ActivitySeverity severity) const;
    void UpdateActivitySummary();
    void ApplyUserPreferences();
    void RefreshPresenceUi();
    void ShowPreferenceResult(const revia::core::PreferenceResult& result);
    // Settings re-reads its values once a burst of preference changes has settled.
    void SchedulePreferenceRefresh();
    // What the window placed into Settings beside the dialog's own rows (the two number rows, the
    // cloud choice and the hosted permission panel), read again when Settings opens.
    void RefreshSettingsExtras();
    void RefreshPreferenceSpins();
    // Saves a number row through the settings worker, but only when it changed since it was read:
    // a spin box reports editingFinished whenever it loses focus.
    void WriteSpinPreference(QSpinBox* spin, const std::string& key, int& stored);
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
    QLabel* stateDot = nullptr;
    QToolButton* statePill = nullptr;
    QLabel* chatTitle = nullptr;
    QLabel* affectLabel = nullptr;
    QLabel* speechLabel = nullptr;
    QLabel* microphoneLabel = nullptr;
    QLabel* automationLabel = nullptr;
    QLabel* visionLabel = nullptr;
    QLabel* perceptionLabel = nullptr;
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
    QWidget* chatPage = nullptr;
    QScrollArea* conversationScroll = nullptr;
    QWidget* archivedBannerHost = nullptr;
    QWidget* archivedChatBanner = nullptr;
    QLabel* archivedChatLabel = nullptr;
    QPushButton* continueChatButton = nullptr;
    std::string viewingArchivedChat;
    QPointer<AnswerFeedbackDialog> answerFeedbackDialog;
    QString latestDisplayedAnswer;
    revia::learning::QualityFeedback displayedAnswerTarget;
    std::uint64_t displayedAnswerTurnId = 0;
    // The transcript entries that one turn's streamed fragments occupy, in order.
    std::uint64_t streamedReplyTurnId = 0;
    std::vector<std::size_t> streamedReplyEntries;
    // The shell (design spec 2.2). Panels and Settings are QObject children of this window, so a
    // findChild from here reaches every page.
    std::unique_ptr<revia::desktop::PanelHost> panelHost;
    revia::desktop::SettingsDialog* settingsDialog = nullptr;
    revia::desktop::CommandPalette* commandPalette = nullptr;
    revia::desktop::ShellSidebar* navigationSidebar = nullptr;
    revia::desktop::ComposerCard* composer = nullptr;
    revia::desktop::ToastOverlay* toast = nullptr;
    QPointer<revia::desktop::ShortcutSheet> shortcutSheet;
    QMenu* sidebarMenu = nullptr;
    QMenu* switchCompanionMenu = nullptr;
    QMenu* sidebarPanelsMenu = nullptr;
    QMenu* chatMoreMenu = nullptr;
    QMenu* composerAddMenu = nullptr;
    QMenu* composerProfileMenu = nullptr;
    QAction* menuNameHeader = nullptr;
    QAction* menuStopDesktop = nullptr;
    QAction* keepOnTopAction = nullptr;
    QAction* useScreenAction = nullptr;
    QWidget* chatColumn = nullptr;
    QWidget* chatHeader = nullptr;
    QWidget* composerArea = nullptr;
    QWidget* composerShadow = nullptr;
    QFrame* statusPopover = nullptr;
    QWidget* privacyIndicators = nullptr;
    QToolButton* privacyMic = nullptr;
    QToolButton* privacyPerception = nullptr;
    QToolButton* privacyScreen = nullptr;
    QToolButton* privacyCamera = nullptr;
    QToolButton* privacyDesktop = nullptr;
    QPushButton* headerDesktopStop = nullptr;
    QToolButton* chatMoreButton = nullptr;
    QToolButton* jumpToLatest = nullptr;
    QToolButton* composerProfileChip = nullptr;
    QToolButton* composerHandsFreeChip = nullptr;
    QLabel* navigationLockOverlay = nullptr;
    QPushButton* runtimeDetailsButton = nullptr;
    QWidget* microphoneRow = nullptr;
    QComboBox* cloudUseForCombo = nullptr;
    std::function<QString(const std::string&)> cloudUseForReader;
    // Turns continuous screen awareness off through the Settings backend, for the header's
    // screen indicator; done gets whether it was saved and the backend's message.
    std::function<void(std::function<void(bool, QString)>)> screenAwarenessOff;
    QTimer* preferenceRefreshTimer = nullptr;
    // The permission panel re-reads the capability file once a burst of capability events settles.
    QTimer* capabilityRefreshTimer = nullptr;
    bool navigationLocked = false;
    bool restartRequested = false;
    int themeListenerId = -1;
    int chatTextSize = 15;
    // Live states the privacy indicators and the state pill show, from runtime events.
    bool perceptionWatching = false;
    bool screenAwarenessRunning = false;
    bool cameraCapturing = false;
    bool automationRunning = false;
    bool microphoneDisabledBySetting = false;
    [[nodiscard]] bool DesktopControlActive() const;
    // Serial writer for the Settings backend; stopped and joined before a session retires.
    std::unique_ptr<revia::desktop::SerialWorker> settingsWorker;
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
    QSpinBox* resourceSampleSpin = nullptr;
    // What the two number rows last read or wrote.
    int initiativeMaxStored = -1;
    int resourceSampleStored = -1;
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
