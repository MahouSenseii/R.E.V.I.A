#pragma once

#include "Core/exitReporter.h"
#include "Runtime/reviaSession.h"

#include <QMainWindow>

#include <atomic>
#include <map>
#include <memory>
#include <thread>
#include <vector>

class QCheckBox;
class QCloseEvent;
class QResizeEvent;
class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
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
namespace Ui { class ReviaWindow; }

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

    explicit ReviaWindow(
        bool startRuntime = true,
        bool buildSystemTray = true,
        QWidget* parent = nullptr);
    ~ReviaWindow() override;
    void RequestShutdown(
        revia::core::ExitReason reason = revia::core::ExitReason::SmokeTest);
    bool IsRuntimeStarted() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void BuildInterface();
    // Keeps the readable measure fixed as the window grows. Without this every row,
    // field, and table column stretches to the full width of the monitor, which is why
    // the same screen looks like a different application maximised.
    void ApplyContentWidthCap();
    void BuildTray();
    void StartRuntime();
    void SendMessage(bool voiceInput = false);
    void ToggleListening();
    void ApplyMicrophoneUi(MicrophoneUi microphoneUi);
    void UseVisibleScreen();
    void DiscoverApplicationPermissions();
    // The voice studio creates and previews voices; which profile speaks with which one
    // is decided in ProfilePanel, so refreshing here refreshes that too.
    void RefreshVoiceStudio();
    void CreateVoicePreset();
    void PreviewVoice();
    // Every shutdown names its own cause, so the ledger never has to guess between a
    // deliberate quit and something that merely looked like one.
    void BeginShutdown(revia::core::ExitReason reason, const std::string& detail = {});
    void ToggleMaximized();
    void UpdateMaximizeButton();
    void SetAlwaysOnTop(bool enabled);
    void HandleRuntimeEvent(const revia::runtime::RuntimeEvent& event);
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
    void AppendChat(
        const QString& speaker,
        const QString& message,
        bool userMessage = false,
        const QString& reasoning = QString());
    // QTextBrowser supports only a subset of HTML and has no <details>, so collapsing is
    // done by re-rendering the whole transcript from this model when a link is clicked.
    struct ChatEntry
    {
        QString speaker;
        QString body;
        QString reasoning;
        bool userMessage = false;
        bool expanded = false;
    };
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
    void AppendActivity(
        const QString& message,
        ActivitySeverity severity = ActivitySeverity::Automatic);
    void RenderActivity();
    void UpdateActivitySummary();
    void ApplyUserPreferences();
    void RefreshPresenceUi();
    void ShowPreferenceResult(const revia::core::PreferenceResult& result);
    bool ConfirmAction(
        const revia::actions::ActionRequest& request,
        const revia::actions::PolicyDecision& decision);
    static QIcon CreateReviaIcon();

    revia::runtime::ReviaSession session;
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
    int activityWarningCount = 0;
    int activityErrorCount = 0;
    PipelinePanel* pipelinePanel = nullptr;
    InternetActivityPanel* internetActivityPanel = nullptr;
    ResourcePanel* resourcePanel = nullptr;
    CanvasPanel* canvasPanel = nullptr;
    CapabilityPanel* capabilityPanel = nullptr;
    ProfilePanel* profilePanel = nullptr;
    MemoryPanel* memoryPanel = nullptr;
    QTabWidget* tabs = nullptr;
    QPushButton* sendButton = nullptr;
    QPushButton* stopButton = nullptr;
    QPushButton* microphoneButton = nullptr;
    QPushButton* screenActionButton = nullptr;
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
    QSystemTrayIcon* trayIcon = nullptr;
    QTimer* pollTimer = nullptr;

    std::jthread operationWorker;
    std::jthread shutdownWorker;
    std::jthread voiceWorker;
    std::jthread capabilityWorker;
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
