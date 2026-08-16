#include "reviaWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QEvent>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPixmap>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextBlockFormat>
#include <QtMath>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <chrono>

namespace
{
    QString StateColor(const revia::runtime::RuntimeState state)
    {
        using revia::runtime::RuntimeState;
        switch (state)
        {
            case RuntimeState::Idle: return "#62e6c8";
            case RuntimeState::Starting:
            case RuntimeState::Thinking:
            case RuntimeState::Responding:
            case RuntimeState::Remembering:
            case RuntimeState::Acting: return "#77b7ff";
            case RuntimeState::WaitingForConfirmation: return "#ffd166";
            case RuntimeState::Blocked:
            case RuntimeState::Error: return "#ff6b7a";
            case RuntimeState::Stopping: return "#c3a6ff";
            case RuntimeState::Offline:
            default: return "#8290a8";
        }
    }

    QString EventTime(const revia::runtime::RuntimeEvent& event)
    {
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            event.occurredAt.time_since_epoch()).count();
        return QDateTime::fromMSecsSinceEpoch(milliseconds).toString("HH:mm:ss.zzz");
    }
}

ReviaWindow::ReviaWindow(
    const bool startRuntime,
    const bool buildSystemTray,
    QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("Revia");
    setWindowIcon(CreateReviaIcon());
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    resize(1040, 720);
    setMinimumSize(760, 540);
    setAttribute(Qt::WA_TranslucentBackground, true);

    BuildInterface();
    if (buildSystemTray)
    {
        BuildTray();
    }

    subscriptionId = session.Events().Subscribe([this](const revia::runtime::RuntimeEvent& event)
    {
        QMetaObject::invokeMethod(this, [this, event]()
        {
            HandleRuntimeEvent(event);
        }, Qt::QueuedConnection);
    });
    session.SetConfirmationHandler([this](
        const revia::actions::ActionRequest& request,
        const revia::actions::PolicyDecision& decision)
    {
        return ConfirmAction(request, decision);
    });

    pollTimer = new QTimer(this);
    pollTimer->setInterval(250);
    connect(pollTimer, &QTimer::timeout, this, [this]()
    {
        session.PollBackgroundEvents();
    });
    pollTimer->start();

    if (startRuntime)
    {
        StartRuntime();
    }
    else
    {
        UpdateState(revia::runtime::RuntimeState::Offline, "UI smoke-test mode");
        sendButton->setEnabled(false);
        stopButton->setEnabled(false);
    }
}

ReviaWindow::~ReviaWindow()
{
    session.Events().Unsubscribe(subscriptionId);
    session.RequestStop();
    if (operationWorker.joinable())
    {
        operationWorker.join();
    }
    session.Stop();
    if (voiceWorker.joinable())
    {
        voiceWorker.join();
    }
}

void ReviaWindow::RequestShutdown()
{
    BeginShutdown();
}

bool ReviaWindow::IsRuntimeStarted() const
{
    return session.IsStarted();
}

bool ReviaWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == titleBar)
    {
        if (event->type() == QEvent::MouseButtonDblClick)
        {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton)
            {
                ToggleMaximized();
                return true;
            }
        }
        if (event->type() == QEvent::MouseButtonPress)
        {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton && windowHandle())
            {
                windowHandle()->startSystemMove();
                return true;
            }
        }
    }

    if (watched == messageInput && event->type() == QEvent::KeyPress)
    {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) &&
            !keyEvent->modifiers().testFlag(Qt::ShiftModifier))
        {
            SendMessage();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void ReviaWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange)
    {
        UpdateMaximizeButton();
    }
}

void ReviaWindow::closeEvent(QCloseEvent* event)
{
    if (!shuttingDown.load() && trayIcon && trayIcon->isVisible())
    {
        hide();
        trayIcon->showMessage(
            "Revia",
            "I'm still running here. Use Quit from the tray when you want me to stop.",
            QSystemTrayIcon::Information,
            2500);
        event->ignore();
        return;
    }
    event->accept();
}

void ReviaWindow::BuildInterface()
{
    auto* root = new QWidget(this);
    root->setObjectName("rootPanel");
    setCentralWidget(root);

    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(12);

    titleBar = new QWidget(root);
    titleBar->setObjectName("titleBar");
    titleBar->setFixedHeight(30);
    titleBar->installEventFilter(this);
    auto* titleBarLayout = new QHBoxLayout(titleBar);
    titleBarLayout->setContentsMargins(2, 0, 0, 0);
    titleBarLayout->setSpacing(6);

    auto* titleIcon = new QLabel(titleBar);
    titleIcon->setPixmap(CreateReviaIcon().pixmap(20, 20));
    titleBarLayout->addWidget(titleIcon);
    auto* windowTitle = new QLabel("Revia", titleBar);
    windowTitle->setObjectName("windowTitle");
    titleBarLayout->addWidget(windowTitle);
    titleBarLayout->addStretch();

    auto* minimizeButton = new QToolButton(titleBar);
    minimizeButton->setObjectName("windowControl");
    minimizeButton->setText(QString::fromUtf8("\xE2\x80\x94"));
    minimizeButton->setToolTip("Minimize");
    maximizeButton = new QToolButton(titleBar);
    maximizeButton->setObjectName("windowControl");
    maximizeButton->setText(QString::fromUtf8("\xE2\x96\xA1"));
    maximizeButton->setToolTip("Maximize");
    auto* closeButton = new QToolButton(titleBar);
    closeButton->setObjectName("closeControl");
    closeButton->setText(QString::fromUtf8("\xC3\x97"));
    closeButton->setToolTip("Close to tray");
    for (QToolButton* button : {minimizeButton, maximizeButton, closeButton})
    {
        button->setFixedSize(40, 28);
        button->setAutoRaise(true);
        titleBarLayout->addWidget(button);
    }
    connect(minimizeButton, &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(maximizeButton, &QToolButton::clicked, this, [this]() { ToggleMaximized(); });
    connect(closeButton, &QToolButton::clicked, this, &QWidget::close);
    layout->addWidget(titleBar);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel("REVIA", root);
    QFont titleFont = title->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setStyleSheet("color: #e8f4ff; letter-spacing: 4px;");
    header->addWidget(title);

    stateLabel = new QLabel("Offline", root);
    stateDetailLabel = new QLabel("Waiting to start", root);
    stateDetailLabel->setStyleSheet("color: #93a4bd; font-size: 11px;");
    header->addStretch();
    header->addWidget(stateLabel);
    header->addWidget(stateDetailLabel);
    layout->addLayout(header);

    auto* statusStrip = new QHBoxLayout();
    statusStrip->setSpacing(8);
    affectLabel = new QLabel("Affect: Neutral", root);
    affectLabel->setObjectName("affectChip");
    speechLabel = new QLabel("Voice: Starting", root);
    speechLabel->setObjectName("voiceChip");
    microphoneLabel = new QLabel("Mic: Starting", root);
    microphoneLabel->setObjectName("micChip");
    automationLabel = new QLabel("Actions: Ready", root);
    automationLabel->setObjectName("automationChip");
    visionLabel = new QLabel("Vision: Starting", root);
    visionLabel->setObjectName("visionChip");
    for (QLabel* chip : {affectLabel, speechLabel, microphoneLabel, automationLabel, visionLabel})
    {
        chip->setMinimumWidth(0);
        chip->setMaximumWidth(180);
        chip->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        chip->setAlignment(Qt::AlignCenter);
        statusStrip->addWidget(chip, 1);
    }
    layout->addLayout(statusStrip);

    tabs = new QTabWidget(root);
    tabs->setDocumentMode(true);
    auto* chatPage = new QWidget(tabs);
    auto* chatLayout = new QVBoxLayout(chatPage);
    chatLayout->setContentsMargins(0, 10, 0, 0);
    chatLayout->setSpacing(10);

    chatHistory = new QTextBrowser(chatPage);
    chatHistory->setOpenExternalLinks(false);
    chatHistory->setPlaceholderText("Revia's conversation will appear here.");
    chatLayout->addWidget(chatHistory, 1);

    messageInput = new QPlainTextEdit(chatPage);
    messageInput->setPlaceholderText("Talk to Revia...  (Shift+Enter for a new line)");
    messageInput->setMaximumHeight(78);
    messageInput->installEventFilter(this);
    chatLayout->addWidget(messageInput);

    auto* buttonRow = new QHBoxLayout();
    sendButton = new QPushButton("Send", chatPage);
    stopButton = new QPushButton("Stop", chatPage);
    stopButton->setObjectName("stopButton");
    stopButton->setEnabled(false);
    microphoneButton = new QPushButton("Hold to talk", chatPage);
    microphoneButton->setObjectName("microphoneButton");
    microphoneButton->setEnabled(false);
    visionButton = new QPushButton("Analyze screen", chatPage);
    visionButton->setObjectName("visionButton");
    visionButton->setEnabled(false);
    for (QPushButton* button : {sendButton, stopButton, microphoneButton, visionButton})
    {
        buttonRow->addWidget(button, 1);
    }
    chatLayout->addLayout(buttonRow);
    tabs->addTab(chatPage, "Chat");

    auto* activityPage = new QWidget(tabs);
    auto* activityLayout = new QVBoxLayout(activityPage);
    activityLayout->setContentsMargins(0, 10, 0, 0);
    activityFeed = new QPlainTextEdit(activityPage);
    activityFeed->setReadOnly(true);
    activityFeed->setMaximumBlockCount(1000);
    activityLayout->addWidget(activityFeed);
    tabs->addTab(activityPage, "Activity");

    auto* voicePage = new QWidget(tabs);
    auto* voiceLayout = new QVBoxLayout(voicePage);
    voiceLayout->setContentsMargins(0, 10, 0, 0);
    voiceLayout->setSpacing(10);
    auto* voiceIntro = new QLabel(
        "Design a voice with Qwen3-TTS, preview it, then assign it to any Revia profile. "
        "Models stay local and download on first use.", voicePage);
    voiceIntro->setWordWrap(true);
    voiceIntro->setObjectName("secondaryText");
    voiceLayout->addWidget(voiceIntro);
    voiceStudioStatus = new QLabel("Voice Studio is ready.", voicePage);
    voiceStudioStatus->setObjectName("voiceStudioStatus");
    voiceStudioStatus->setWordWrap(true);
    voiceLayout->addWidget(voiceStudioStatus);

    auto* assignmentForm = new QFormLayout();
    assignmentForm->setHorizontalSpacing(12);
    voiceProfileCombo = new QComboBox(voicePage);
    voicePresetCombo = new QComboBox(voicePage);
    assignmentForm->addRow("Profile", voiceProfileCombo);
    assignmentForm->addRow("Assigned voice", voicePresetCombo);
    voiceLayout->addLayout(assignmentForm);
    auto* assignmentButtons = new QHBoxLayout();
    assignVoiceButton = new QPushButton("Assign voice", voicePage);
    fallbackVoiceButton = new QPushButton("Use Windows fallback", voicePage);
    fallbackVoiceButton->setObjectName("secondaryButton");
    assignmentButtons->addWidget(assignVoiceButton);
    assignmentButtons->addWidget(fallbackVoiceButton);
    assignmentButtons->addStretch();
    voiceLayout->addLayout(assignmentButtons);

    auto* studioForm = new QFormLayout();
    studioForm->setHorizontalSpacing(12);
    voiceNameInput = new QLineEdit(voicePage);
    voiceNameInput->setPlaceholderText("Revia Bright");
    voiceLanguageCombo = new QComboBox(voicePage);
    voiceLanguageCombo->addItems({"English", "Chinese", "Japanese", "Korean", "German",
        "French", "Russian", "Portuguese", "Spanish", "Italian"});
    voiceDescriptionInput = new QPlainTextEdit(voicePage);
    voiceDescriptionInput->setMaximumHeight(86);
    voiceDescriptionInput->setPlaceholderText(
        "A bright youthful synthetic voice, curious and clear, gently playful, never babyish.");
    voiceReferenceInput = new QPlainTextEdit(voicePage);
    voiceReferenceInput->setMaximumHeight(64);
    voiceReferenceInput->setPlainText(
        "Wait, I found it. The pattern is clearer now, and I think this part matters.");
    studioForm->addRow("Preset name", voiceNameInput);
    studioForm->addRow("Language", voiceLanguageCombo);
    studioForm->addRow("Voice description", voiceDescriptionInput);
    studioForm->addRow("Reference line", voiceReferenceInput);
    voiceLayout->addLayout(studioForm);
    createVoiceButton = new QPushButton("Create voice", voicePage);
    voiceLayout->addWidget(createVoiceButton, 0, Qt::AlignLeft);

    auto* previewRow = new QHBoxLayout();
    voicePreviewInput = new QPlainTextEdit(voicePage);
    voicePreviewInput->setMaximumHeight(62);
    voicePreviewInput->setPlainText("Hi. I'm Revia. Oh, this voice fits much better.");
    previewVoiceButton = new QPushButton("Generate preview", voicePage);
    previewRow->addWidget(voicePreviewInput, 1);
    previewRow->addWidget(previewVoiceButton);
    voiceLayout->addLayout(previewRow);
    voiceLayout->addStretch();
    tabs->addTab(voicePage, "Voice Studio");

    auto* settingsPage = new QWidget(tabs);
    auto* settingsLayout = new QVBoxLayout(settingsPage);
    settingsLayout->setContentsMargins(0, 14, 0, 0);
    auto* settingsTitle = new QLabel("Window and speech", settingsPage);
    settingsTitle->setObjectName("sectionTitle");
    settingsLayout->addWidget(settingsTitle);

    auto* preferenceRow = new QHBoxLayout();
    alwaysOnTopCheck = new QCheckBox("Keep Revia on top", settingsPage);
    speechCheck = new QCheckBox("Speak replies", settingsPage);
    speechCheck->setChecked(true);
    preferenceRow->addWidget(alwaysOnTopCheck);
    preferenceRow->addWidget(speechCheck);
    preferenceRow->addStretch();
    settingsLayout->addLayout(preferenceRow);
    auto* settingsHint = new QLabel(
        "Auto voice mode uses a profile's Qwen3-TTS preset when assigned and keeps Windows "
        "SAPI as a dependable fallback. Hardware selection is re-evaluated on each model load.",
        settingsPage);
    settingsHint->setWordWrap(true);
    settingsHint->setObjectName("secondaryText");
    settingsLayout->addWidget(settingsHint);
    settingsLayout->addStretch();
    tabs->addTab(settingsPage, "Settings");
    layout->addWidget(tabs, 1);

    connect(sendButton, &QPushButton::clicked, this, [this]() { SendMessage(); });
    connect(stopButton, &QPushButton::clicked, this, [this]() { session.RequestStop(); });
    connect(microphoneButton, &QPushButton::pressed, this, [this]()
    {
        if (session.BeginListening())
        {
            microphoneButton->setText("Listening...");
        }
    });
    connect(microphoneButton, &QPushButton::released, this, [this]()
    {
        session.EndListening();
        microphoneButton->setText("Hold to talk");
    });
    connect(visionButton, &QPushButton::clicked, this, [this]() { AnalyzeVisibleScreen(); });
    connect(alwaysOnTopCheck, &QCheckBox::toggled, this, [this](const bool enabled)
    {
        SetAlwaysOnTop(enabled);
    });
    connect(speechCheck, &QCheckBox::toggled, this, [this](const bool enabled)
    {
        session.SetSpeechEnabled(enabled);
    });
    connect(createVoiceButton, &QPushButton::clicked, this, [this]() { CreateVoicePreset(); });
    connect(previewVoiceButton, &QPushButton::clicked, this, [this]() { PreviewVoice(); });
    connect(assignVoiceButton, &QPushButton::clicked, this, [this]() { AssignVoice(); });
    connect(fallbackVoiceButton, &QPushButton::clicked, this, [this]() { AssignVoice(true); });
    connect(voiceProfileCombo, &QComboBox::currentTextChanged, this, [this](const QString& profile)
    {
        const auto snapshot = session.VoiceStudio();
        const auto found = snapshot.profileAssignments.find(profile.toStdString());
        const std::string assigned = found == snapshot.profileAssignments.end()
            ? std::string()
            : found->second;
        if (!assigned.empty())
        {
            const int index = voicePresetCombo->findData(QString::fromStdString(assigned));
            if (index >= 0)
            {
                voicePresetCombo->setCurrentIndex(index);
            }
        }
        else if (voicePresetCombo->count() > 0)
        {
            voicePresetCombo->setCurrentIndex(0);
        }
    });
    RefreshVoiceStudio();

    setStyleSheet(R"(
        QWidget#rootPanel {
            background: rgba(14, 20, 34, 242);
            border-radius: 16px;
        }
        QWidget#titleBar { background: transparent; }
        QLabel#windowTitle { color: #93a4bd; font-size: 12px; }
        QLabel#affectChip, QLabel#voiceChip, QLabel#micChip,
        QLabel#automationChip, QLabel#visionChip {
            background: rgba(25, 36, 55, 205);
            border: 1px solid #293b55;
            border-radius: 8px;
            padding: 6px 7px;
            font-size: 10px;
        }
        QLabel#affectChip { color: #c3a6ff; }
        QLabel#voiceChip { color: #85d7c8; }
        QLabel#micChip { color: #8ac7ff; }
        QLabel#automationChip { color: #e0bd75; }
        QLabel#visionChip { color: #e69bc4; }
        QLabel#secondaryText { color: #93a4bd; }
        QLabel#sectionTitle { color: #dce9f7; font-size: 15px; font-weight: 700; }
        QLabel#voiceStudioStatus {
            color: #85d7c8;
            background: rgba(20, 48, 57, 180);
            border: 1px solid #2f625f;
            border-radius: 8px;
            padding: 8px;
        }
        QTabWidget::pane { border: none; background: transparent; }
        QTabBar::tab {
            color: #93a4bd;
            background: transparent;
            border: none;
            border-bottom: 2px solid transparent;
            padding: 9px 18px;
            margin-right: 3px;
            font-weight: 600;
        }
        QTabBar::tab:hover { color: #dce9f7; background: rgba(35, 54, 78, 100); }
        QTabBar::tab:selected { color: #70e0ca; border-bottom-color: #70e0ca; }
        QComboBox, QLineEdit {
            color: #dce9f7;
            background: rgba(7, 12, 23, 220);
            border: 1px solid #293b55;
            border-radius: 8px;
            padding: 8px;
            selection-background-color: #315f84;
        }
        QComboBox QAbstractItemView {
            color: #dce9f7;
            background: #111b2d;
            border: 1px solid #38506e;
            selection-background-color: #315f84;
        }
        QToolButton#windowControl, QToolButton#closeControl {
            color: #b8c6d9;
            background: transparent;
            border: none;
            border-radius: 6px;
            font-size: 16px;
        }
        QToolButton#windowControl:hover { background: rgba(93, 119, 151, 90); color: white; }
        QToolButton#closeControl:hover { background: #c94b5f; color: white; }
        QTextBrowser, QPlainTextEdit {
            background: rgba(7, 12, 23, 220);
            color: #dce9f7;
            border: 1px solid #293b55;
            border-radius: 10px;
            padding: 9px;
            selection-background-color: #315f84;
        }
        QPushButton {
            background: #327aa8;
            color: white;
            border: none;
            border-radius: 8px;
            padding: 10px 18px;
            font-weight: 600;
        }
        QPushButton:hover { background: #4294c8; }
        QPushButton:disabled { background: #354154; color: #8390a3; }
        QPushButton#stopButton { background: #7d3d55; }
        QPushButton#stopButton:hover { background: #a64d69; }
        QPushButton#microphoneButton { background: #31556f; }
        QPushButton#microphoneButton:pressed { background: #2b9a8b; }
        QPushButton#visionButton { background: #694663; }
        QPushButton#visionButton:hover { background: #8e5b84; }
        QPushButton#secondaryButton { background: #354154; }
        QPushButton#secondaryButton:hover { background: #46556d; }
        QCheckBox { color: #a9bad2; }
        QToolTip { color: white; background: #182438; border: 1px solid #45607f; }
    )");
}

void ReviaWindow::BuildTray()
{
    trayIcon = new QSystemTrayIcon(CreateReviaIcon(), this);
    auto* trayMenu = new QMenu(this);
    QAction* openAction = trayMenu->addAction("Open Revia");
    QAction* alwaysOnTopAction = trayMenu->addAction("Always on top");
    alwaysOnTopAction->setCheckable(true);
    trayMenu->addSeparator();
    QAction* quitAction = trayMenu->addAction("Quit");
    trayIcon->setContextMenu(trayMenu);
    trayIcon->setToolTip("Revia - Offline");

    connect(openAction, &QAction::triggered, this, [this]()
    {
        showNormal();
        raise();
        activateWindow();
    });
    connect(alwaysOnTopAction, &QAction::toggled, alwaysOnTopCheck, &QCheckBox::setChecked);
    connect(alwaysOnTopCheck, &QCheckBox::toggled, alwaysOnTopAction, &QAction::setChecked);
    connect(quitAction, &QAction::triggered, this, [this]() { BeginShutdown(); });
    connect(trayIcon, &QSystemTrayIcon::activated, this, [this](const QSystemTrayIcon::ActivationReason reason)
    {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
        {
            showNormal();
            raise();
            activateWindow();
        }
    });
    trayIcon->show();
}

void ReviaWindow::StartRuntime()
{
    sendButton->setEnabled(false);
    stopButton->setEnabled(true);
    operationWorker = std::jthread([this]()
    {
        const bool started = session.Start();
        const QString greeting = QString::fromStdString(session.Greeting());
        QMetaObject::invokeMethod(this, [this, started, greeting]()
        {
            sendButton->setEnabled(started);
            stopButton->setEnabled(false);
            if (!greeting.isEmpty())
            {
                AppendChat(QString::fromStdString(session.DisplayName()), greeting);
            }
            if (!started)
            {
                AppendActivity("Startup did not complete. Check the activity log.");
            }
            RefreshVoiceStudio();
            messageInput->setFocus();
        }, Qt::QueuedConnection);
    });
}

void ReviaWindow::SendMessage()
{
    if (shuttingDown.load() || session.IsBusy())
    {
        return;
    }

    const QString text = messageInput->toPlainText().trimmed();
    if (text.isEmpty())
    {
        return;
    }
    messageInput->clear();
    AppendChat("You", text, true);
    sendButton->setEnabled(false);
    stopButton->setEnabled(true);

    if (operationWorker.joinable())
    {
        operationWorker.join();
    }
    operationWorker = std::jthread([this, input = text.toStdString()]()
    {
        const revia::runtime::SessionResult result = session.Submit(input);
        QMetaObject::invokeMethod(this, [this, result]()
        {
            if (!result.text.empty())
            {
                AppendChat(
                    result.fromAssistant ? QString::fromStdString(session.DisplayName()) : "System",
                    QString::fromStdString(result.text));
            }
            if (!result.succeeded && !result.reason.empty())
            {
                AppendActivity("Request stopped: " + QString::fromStdString(result.reason));
            }
            sendButton->setEnabled(!result.shouldExit && session.IsStarted());
            stopButton->setEnabled(false);
            if (result.shouldExit)
            {
                BeginShutdown();
            }
            else
            {
                messageInput->setFocus();
            }
        }, Qt::QueuedConnection);
    });
}

void ReviaWindow::BeginShutdown()
{
    if (shuttingDown.exchange(true))
    {
        return;
    }
    sendButton->setEnabled(false);
    stopButton->setEnabled(false);
    session.RequestStop();
    shutdownWorker = std::jthread([this]()
    {
        session.Stop();
        QMetaObject::invokeMethod(qApp, []() { QApplication::quit(); }, Qt::QueuedConnection);
    });
}

void ReviaWindow::AnalyzeVisibleScreen()
{
    if (shuttingDown.load() || session.IsBusy())
    {
        return;
    }
    const bool approved = QMessageBox::question(
        this,
        "Share screen with Revia",
        "Revia will capture all visible monitors once and analyze the image locally. "
        "The temporary PNG is deleted after the request. Continue?",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No) == QMessageBox::Yes;
    if (!approved)
    {
        return;
    }
    showMinimized();
    sendButton->setEnabled(false);
    visionButton->setEnabled(false);
    stopButton->setEnabled(true);
    if (operationWorker.joinable())
    {
        operationWorker.join();
    }
    operationWorker = std::jthread([this]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        const revia::runtime::SessionResult result = session.AnalyzeScreen(
            "Describe what is visible on the screen accurately and briefly. "
            "Mention errors, warnings, or the most useful next action if one is visible.");
        QMetaObject::invokeMethod(this, [this, result]()
        {
            showNormal();
            raise();
            activateWindow();
            if (!result.text.empty())
            {
                AppendChat(QString::fromStdString(session.DisplayName()),
                    QString::fromStdString(result.text));
            }
            if (!result.succeeded && !result.reason.empty())
            {
                AppendActivity("Vision failed: " + QString::fromStdString(result.reason));
            }
            sendButton->setEnabled(session.IsStarted());
            stopButton->setEnabled(false);
            messageInput->setFocus();
        }, Qt::QueuedConnection);
    });
}

void ReviaWindow::RefreshVoiceStudio()
{
    const revia::speech::VoiceStudioSnapshot snapshot = session.VoiceStudio();
    const QString selectedProfile = voiceProfileCombo->currentText();
    const QString selectedPreset = voicePresetCombo->currentData().toString();
    {
        const QSignalBlocker profileBlocker(voiceProfileCombo);
        voiceProfileCombo->clear();
        for (const std::string& profileId : snapshot.profiles)
        {
            voiceProfileCombo->addItem(QString::fromStdString(profileId));
        }
        int profileIndex = voiceProfileCombo->findText(
            selectedProfile.isEmpty()
                ? QString::fromStdString(snapshot.activeProfile)
                : selectedProfile);
        if (profileIndex < 0 && voiceProfileCombo->count() > 0)
        {
            profileIndex = 0;
        }
        voiceProfileCombo->setCurrentIndex(profileIndex);
    }
    {
        const QSignalBlocker presetBlocker(voicePresetCombo);
        voicePresetCombo->clear();
        voicePresetCombo->addItem("Select a created voice...", QString());
        for (const auto& preset : snapshot.presets)
        {
            voicePresetCombo->addItem(
                QString::fromStdString(preset.name),
                QString::fromStdString(preset.id));
        }
        QString targetPreset = selectedPreset;
        const auto assignment = snapshot.profileAssignments.find(
            voiceProfileCombo->currentText().toStdString());
        if (assignment != snapshot.profileAssignments.end())
        {
            targetPreset = QString::fromStdString(assignment->second);
        }
        const int presetIndex = voicePresetCombo->findData(targetPreset);
        voicePresetCombo->setCurrentIndex(presetIndex >= 0 ? presetIndex : 0);
    }
    if (!voiceOperationRunning.load())
    {
        const QString assigned = voicePresetCombo->currentData().toString();
        voiceStudioStatus->setText(assigned.isEmpty()
            ? "This profile uses the Windows voice fallback. Create or select a Qwen voice to assign it."
            : "Profile voice: " + voicePresetCombo->currentText() +
                ". Preview it or replace the assignment at any time.");
    }
}

void ReviaWindow::CreateVoicePreset()
{
    if (voiceOperationRunning.exchange(true))
    {
        return;
    }
    const QString name = voiceNameInput->text().trimmed();
    const QString description = voiceDescriptionInput->toPlainText().trimmed();
    const QString reference = voiceReferenceInput->toPlainText().trimmed();
    const QString language = voiceLanguageCombo->currentText();
    if (name.isEmpty() || description.isEmpty() || reference.isEmpty())
    {
        voiceOperationRunning.store(false);
        voiceStudioStatus->setText(
            "Enter a preset name, voice description, and reference line before creating a voice.");
        return;
    }
    for (QPushButton* button : {
        createVoiceButton, previewVoiceButton, assignVoiceButton, fallbackVoiceButton})
    {
        button->setEnabled(false);
    }
    voiceStudioStatus->setText(
        "Creating the Qwen3-TTS voice. The first run downloads about 4.5 GB of model files; "
        "progress and hardware choice are written to Activity and Logs/qwen-tts.stdout.log.");
    if (voiceWorker.joinable())
    {
        voiceWorker.join();
    }
    voiceWorker = std::jthread([this, name, description, reference, language]()
    {
        const revia::speech::VoiceOperationResult result = session.CreateVoicePreset(
            name.toStdString(), description.toStdString(), reference.toStdString(),
            language.toStdString());
        QMetaObject::invokeMethod(this, [this, result, name]()
        {
            voiceOperationRunning.store(false);
            for (QPushButton* button : {
                createVoiceButton, previewVoiceButton, assignVoiceButton, fallbackVoiceButton})
            {
                button->setEnabled(true);
            }
            RefreshVoiceStudio();
            const int createdIndex = voicePresetCombo->findText(name);
            if (createdIndex >= 0)
            {
                voicePresetCombo->setCurrentIndex(createdIndex);
            }
            voiceStudioStatus->setText(QString::fromStdString(result.message));
            if (!result.succeeded)
            {
                tabs->setCurrentIndex(tabs->indexOf(voiceStudioStatus->parentWidget()));
            }
        }, Qt::QueuedConnection);
    });
}

void ReviaWindow::PreviewVoice()
{
    if (voiceOperationRunning.exchange(true))
    {
        return;
    }
    const QString presetId = voicePresetCombo->currentData().toString();
    const QString preview = voicePreviewInput->toPlainText().trimmed();
    if (presetId.isEmpty() || preview.isEmpty())
    {
        voiceOperationRunning.store(false);
        voiceStudioStatus->setText("Select a created voice and enter a preview line first.");
        return;
    }
    for (QPushButton* button : {
        createVoiceButton, previewVoiceButton, assignVoiceButton, fallbackVoiceButton})
    {
        button->setEnabled(false);
    }
    voiceStudioStatus->setText(
        "Generating the preview with Qwen3-TTS. The Base model downloads on first use.");
    if (voiceWorker.joinable())
    {
        voiceWorker.join();
    }
    voiceWorker = std::jthread([this, presetId, preview]()
    {
        const revia::speech::VoiceOperationResult result = session.PreviewVoice(
            presetId.toStdString(), preview.toStdString());
        QMetaObject::invokeMethod(this, [this, result]()
        {
            voiceOperationRunning.store(false);
            for (QPushButton* button : {
                createVoiceButton, previewVoiceButton, assignVoiceButton, fallbackVoiceButton})
            {
                button->setEnabled(true);
            }
            voiceStudioStatus->setText(QString::fromStdString(result.message));
        }, Qt::QueuedConnection);
    });
}

void ReviaWindow::AssignVoice(const bool useFallback)
{
    if (voiceOperationRunning.load())
    {
        return;
    }
    const QString profileId = voiceProfileCombo->currentText();
    const QString presetId = useFallback ? QString() : voicePresetCombo->currentData().toString();
    if (profileId.isEmpty() || (!useFallback && presetId.isEmpty()))
    {
        voiceStudioStatus->setText("Choose a profile and created voice first.");
        return;
    }
    const revia::speech::VoiceOperationResult result = session.AssignVoice(
        profileId.toStdString(), presetId.toStdString());
    RefreshVoiceStudio();
    voiceStudioStatus->setText(QString::fromStdString(result.message));
}

void ReviaWindow::ToggleMaximized()
{
    if (isMaximized())
    {
        showNormal();
    }
    else
    {
        showMaximized();
    }
    UpdateMaximizeButton();
}

void ReviaWindow::UpdateMaximizeButton()
{
    if (!maximizeButton)
    {
        return;
    }
    maximizeButton->setText(isMaximized()
        ? QString::fromUtf8("\xE2\x9D\x90")
        : QString::fromUtf8("\xE2\x96\xA1"));
    maximizeButton->setToolTip(isMaximized() ? "Restore" : "Maximize");
}

void ReviaWindow::SetAlwaysOnTop(const bool enabled)
{
    const QRect previousGeometry = geometry();
    setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
    show();
    setGeometry(previousGeometry);
}

void ReviaWindow::HandleRuntimeEvent(const revia::runtime::RuntimeEvent& event)
{
    if (event.kind == revia::runtime::RuntimeEventKind::StateChanged)
    {
        UpdateState(event.state, QString::fromStdString(event.message));
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::Memory)
    {
        AppendActivity(QString::fromStdString(event.message));
        AppendChat("Memory", QString::fromStdString(event.message));
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::AffectChanged)
    {
        const int percent = qRound(event.affectIntensity * 100.0F);
        affectLabel->setText(
            QStringLiteral("Affect: ") +
            QString::fromStdString(revia::runtime::ToString(event.affect)) +
            QStringLiteral(" ") + QString::number(percent) + QStringLiteral("%"));
        affectLabel->setToolTip(QString::fromStdString(event.message));
        AppendActivity(
            QStringLiteral("Affect -> ") +
            QString::fromStdString(revia::runtime::ToString(event.affect)) +
            QStringLiteral(" (") + QString::number(percent) + QStringLiteral("%): ") +
            QString::fromStdString(event.message));
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::ComponentStatus)
    {
        if (event.component == "Voice")
        {
            const QString phase = QString::fromStdString(event.phase);
            speechLabel->setText(QStringLiteral("Voice: ") + phase);
            speechLabel->setToolTip(QString::fromStdString(event.message));
            speechActive = phase == QStringLiteral("Queued") ||
                phase == QStringLiteral("Loading") || phase == QStringLiteral("Designing") ||
                phase == QStringLiteral("Generating") || phase == QStringLiteral("Speaking");
            {
                const QSignalBlocker blocker(speechCheck);
                speechCheck->setChecked(phase != QStringLiteral("Disabled") &&
                    phase != QStringLiteral("Unavailable") && phase != QStringLiteral("Error"));
            }
            QString detail = QStringLiteral("Voice ") + phase + QStringLiteral(": ") +
                QString::fromStdString(event.message);
            if (event.elapsedMilliseconds >= 0.0)
            {
                detail += QStringLiteral(" (") +
                    QString::number(event.elapsedMilliseconds, 'f', 1) + QStringLiteral("ms)");
            }
            if (event.queueDepth > 0)
            {
                detail += QStringLiteral(" queue=") + QString::number(event.queueDepth);
            }
            AppendActivity(detail);
            stopButton->setEnabled(speechActive || session.IsBusy());
        }
        else if (event.component == "Microphone")
        {
            const QString phase = QString::fromStdString(event.phase);
            microphoneLabel->setText(QStringLiteral("Mic: ") + phase);
            microphoneLabel->setToolTip(QString::fromStdString(event.message));
            microphoneActive = phase == QStringLiteral("Recording") ||
                phase == QStringLiteral("Captured") || phase == QStringLiteral("Transcribing");
            microphoneButton->setEnabled(
                phase != QStringLiteral("Missing") && phase != QStringLiteral("Disabled") &&
                phase != QStringLiteral("Unavailable") && phase != QStringLiteral("Error") &&
                !microphoneActive);
            if (phase == QStringLiteral("Recording"))
            {
                microphoneButton->setEnabled(true);
            }
            if (phase == QStringLiteral("Transcript"))
            {
                const QString transcript = QString::fromStdString(event.message).trimmed();
                messageInput->setPlainText(transcript);
                messageInput->setFocus();
                microphoneButton->setEnabled(true);
            }
            QString detail = QStringLiteral("Microphone ") + phase + QStringLiteral(": ") +
                QString::fromStdString(event.message);
            if (event.elapsedMilliseconds >= 0.0)
            {
                detail += QStringLiteral(" (") +
                    QString::number(event.elapsedMilliseconds, 'f', 1) + QStringLiteral("ms)");
            }
            AppendActivity(detail);
            stopButton->setEnabled(microphoneActive || speechActive || session.IsBusy());
        }
        else if (event.component == "Automation")
        {
            const QString phase = QString::fromStdString(event.phase);
            automationLabel->setText(QStringLiteral("Actions: ") + phase);
            automationLabel->setToolTip(QString::fromStdString(event.message));
            QString detail = QStringLiteral("Automation ") + phase + QStringLiteral(": ") +
                QString::fromStdString(event.message);
            if (event.elapsedMilliseconds >= 0.0)
            {
                detail += QStringLiteral(" (") +
                    QString::number(event.elapsedMilliseconds, 'f', 1) + QStringLiteral("ms)");
            }
            AppendActivity(detail);
        }
        else if (event.component == "Vision")
        {
            const QString phase = QString::fromStdString(event.phase);
            visionLabel->setText(QStringLiteral("Vision: ") + phase);
            visionLabel->setToolTip(QString::fromStdString(event.message));
            visionButton->setEnabled(
                phase != QStringLiteral("Disabled") && phase != QStringLiteral("Unavailable") &&
                phase != QStringLiteral("Error") && phase != QStringLiteral("Capturing") &&
                phase != QStringLiteral("Analyzing") && !session.IsBusy());
            QString detail = QStringLiteral("Vision ") + phase + QStringLiteral(": ") +
                QString::fromStdString(event.message);
            if (event.elapsedMilliseconds >= 0.0)
            {
                detail += QStringLiteral(" (") +
                    QString::number(event.elapsedMilliseconds, 'f', 1) + QStringLiteral("ms)");
            }
            AppendActivity(detail);
        }
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::AssistantMessage)
    {
        return;
    }

    const QString message = QString::fromStdString(event.message);
    AppendActivity(message.startsWith('[')
        ? message
        : '[' + EventTime(event) + "] " + message);
}

void ReviaWindow::UpdateState(
    const revia::runtime::RuntimeState newState,
    const QString& detail)
{
    const QString name = QString::fromStdString(revia::runtime::ToString(newState));
    const QString color = StateColor(newState);
    stateLabel->setText("●  " + name);
    stateLabel->setStyleSheet("color: " + color + "; font-weight: 700;");
    const QString cleanDetail = detail.trimmed().compare(name, Qt::CaseInsensitive) == 0
        ? QString()
        : detail.trimmed();
    stateDetailLabel->setText(cleanDetail);
    stateDetailLabel->setVisible(!cleanDetail.isEmpty());
    if (trayIcon)
    {
        trayIcon->setToolTip("Revia - " + name);
    }

    const bool cancellable = newState == revia::runtime::RuntimeState::Starting ||
        newState == revia::runtime::RuntimeState::Thinking ||
        newState == revia::runtime::RuntimeState::Responding ||
        newState == revia::runtime::RuntimeState::Acting;
    stopButton->setEnabled(cancellable || speechActive || microphoneActive);
}

void ReviaWindow::AppendChat(
    const QString& speaker,
    const QString& message,
    const bool userMessage)
{
    QTextCursor cursor(chatHistory->document());
    cursor.movePosition(QTextCursor::End);

    QTextBlockFormat blockFormat;
    blockFormat.setAlignment(userMessage ? Qt::AlignRight : Qt::AlignLeft);
    blockFormat.setTopMargin(8.0);
    blockFormat.setBottomMargin(4.0);
    if (chatHistory->document()->isEmpty())
    {
        cursor.setBlockFormat(blockFormat);
    }
    else
    {
        cursor.insertBlock(blockFormat);
    }

    QTextCharFormat speakerFormat;
    speakerFormat.setFontWeight(QFont::Bold);
    speakerFormat.setForeground(QColor(userMessage ? "#9dd7ff" : "#70e0ca"));
    cursor.insertText(speaker, speakerFormat);
    cursor.insertText(QString(QChar::LineSeparator));

    QTextCharFormat messageFormat;
    messageFormat.setForeground(QColor("#dce9f7"));
    QString visibleMessage = message;
    visibleMessage.replace("\r\n", "\n");
    visibleMessage.replace('\r', '\n');
    visibleMessage.replace('\n', QChar::LineSeparator);
    cursor.insertText(visibleMessage, messageFormat);

    chatHistory->setTextCursor(cursor);
    chatHistory->verticalScrollBar()->setValue(chatHistory->verticalScrollBar()->maximum());
}

void ReviaWindow::AppendActivity(const QString& message)
{
    activityFeed->appendPlainText(message);
    activityFeed->verticalScrollBar()->setValue(activityFeed->verticalScrollBar()->maximum());
}

bool ReviaWindow::ConfirmAction(
    const revia::actions::ActionRequest& request,
    const revia::actions::PolicyDecision& decision)
{
    bool confirmed = false;
    const auto showConfirmation = [this, &request, &decision, &confirmed]()
    {
        QString description = "Action: " + QString::fromStdString(revia::actions::ToString(request.type)) +
            (request.application.empty()
                ? "\nSource: " + QString::fromStdString(revia::actions::PathToUtf8(request.source))
                : "\nApplication: " + QString::fromStdString(request.application) +
                    "\nWindow: " + QString::fromStdString(request.windowTitle) +
                    "\nControl: " + QString::fromStdString(request.control));
        if (!request.destination.empty())
        {
            description += "\nDestination: " +
                QString::fromStdString(revia::actions::PathToUtf8(request.destination));
        }
        description += "\n\nPolicy: " + QString::fromStdString(decision.reason);
        confirmed = QMessageBox::question(
            this,
            "Confirm Revia action",
            description,
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
    };

    if (QThread::currentThread() == thread())
    {
        showConfirmation();
    }
    else
    {
        QMetaObject::invokeMethod(this, showConfirmation, Qt::BlockingQueuedConnection);
    }
    return confirmed;
}

QIcon ReviaWindow::CreateReviaIcon()
{
    QPixmap image(64, 64);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor("#16263d"));
    painter.setPen(QPen(QColor("#6fcfe8"), 3));
    painter.drawEllipse(4, 4, 56, 56);
    QFont font("Segoe UI", 28, QFont::Bold);
    painter.setFont(font);
    painter.setPen(QColor("#70e0ca"));
    painter.drawText(image.rect(), Qt::AlignCenter, "R");
    return QIcon(image);
}
