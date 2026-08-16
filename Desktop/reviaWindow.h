#pragma once

#include "Runtime/reviaSession.h"

#include <QMainWindow>

#include <atomic>
#include <thread>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSystemTrayIcon;
class QTabWidget;
class QTextBrowser;
class QTimer;
class QToolButton;
class QWidget;

class ReviaWindow final : public QMainWindow
{
public:
    explicit ReviaWindow(
        bool startRuntime = true,
        bool buildSystemTray = true,
        QWidget* parent = nullptr);
    ~ReviaWindow() override;
    void RequestShutdown();
    bool IsRuntimeStarted() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void BuildInterface();
    void BuildTray();
    void StartRuntime();
    void SendMessage();
    void AnalyzeVisibleScreen();
    void RefreshVoiceStudio();
    void CreateVoicePreset();
    void PreviewVoice();
    void AssignVoice(bool useFallback = false);
    void BeginShutdown();
    void ToggleMaximized();
    void UpdateMaximizeButton();
    void SetAlwaysOnTop(bool enabled);
    void HandleRuntimeEvent(const revia::runtime::RuntimeEvent& event);
    void UpdateState(revia::runtime::RuntimeState state, const QString& detail);
    void AppendChat(const QString& speaker, const QString& message, bool userMessage = false);
    void AppendActivity(const QString& message);
    bool ConfirmAction(
        const revia::actions::ActionRequest& request,
        const revia::actions::PolicyDecision& decision);
    static QIcon CreateReviaIcon();

    revia::runtime::ReviaSession session;
    revia::runtime::RuntimeEventBus::SubscriptionId subscriptionId = 0;

    QLabel* stateLabel = nullptr;
    QLabel* stateDetailLabel = nullptr;
    QLabel* affectLabel = nullptr;
    QLabel* speechLabel = nullptr;
    QLabel* microphoneLabel = nullptr;
    QLabel* automationLabel = nullptr;
    QLabel* visionLabel = nullptr;
    QWidget* titleBar = nullptr;
    QToolButton* maximizeButton = nullptr;
    QTextBrowser* chatHistory = nullptr;
    QPlainTextEdit* messageInput = nullptr;
    QPlainTextEdit* activityFeed = nullptr;
    QTabWidget* tabs = nullptr;
    QPushButton* sendButton = nullptr;
    QPushButton* stopButton = nullptr;
    QPushButton* microphoneButton = nullptr;
    QPushButton* visionButton = nullptr;
    QCheckBox* alwaysOnTopCheck = nullptr;
    QCheckBox* speechCheck = nullptr;
    QComboBox* voiceProfileCombo = nullptr;
    QComboBox* voicePresetCombo = nullptr;
    QComboBox* voiceLanguageCombo = nullptr;
    QLineEdit* voiceNameInput = nullptr;
    QPlainTextEdit* voiceDescriptionInput = nullptr;
    QPlainTextEdit* voiceReferenceInput = nullptr;
    QPlainTextEdit* voicePreviewInput = nullptr;
    QLabel* voiceStudioStatus = nullptr;
    QPushButton* createVoiceButton = nullptr;
    QPushButton* previewVoiceButton = nullptr;
    QPushButton* assignVoiceButton = nullptr;
    QPushButton* fallbackVoiceButton = nullptr;
    QSystemTrayIcon* trayIcon = nullptr;
    QTimer* pollTimer = nullptr;

    std::jthread operationWorker;
    std::jthread shutdownWorker;
    std::jthread voiceWorker;
    std::atomic<bool> shuttingDown = false;
    std::atomic<bool> voiceOperationRunning = false;
    bool speechActive = false;
    bool microphoneActive = false;
};
