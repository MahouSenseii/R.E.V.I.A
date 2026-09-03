#include "reviaWindow.h"

#include "Runtime/retainedCounts.h"
#include "Speech/transcriptRouting.h"
#include "capabilityPanel.h"
#include "pipelinePanel.h"
#include "memoryPanel.h"
#include "mindPanel.h"
#include "visionPanel.h"
#include "profilePanel.h"
#include "canvasPanel.h"
#include "internetActivityPanel.h"
#include "resourcePanel.h"
#include "ui_reviaWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFont>
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
#include <QResizeEvent>
#include <QProgressBar>
#include <QPixmap>
#include <QScrollBar>
#include <QSettings>
#include <QStatusBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTextBrowser>
#include <QTextBlockFormat>
#include <QtMath>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QWindow>

#include <algorithm>
#include <chrono>
#include <cstddef>

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

    // Escapes text and turns line breaks into <br>, so a reply can never inject markup
    // into the transcript and its own paragraphing survives.
    QString HtmlParagraph(const QString& text)
    {
        QString html = text.toHtmlEscaped();
        html.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        html.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        html.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        return html;
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
    : QMainWindow(parent), ui(std::make_unique<Ui::ReviaWindow>())
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

    // Upper bound on how long a reply waits for its audio. Qwen generation can take
    // seconds; silence for longer than this reads as the reply having been lost.
    pendingSpeechTimer = new QTimer(this);
    pendingSpeechTimer->setSingleShot(true);
    pendingSpeechTimer->setInterval(9000);
    connect(pendingSpeechTimer, &QTimer::timeout, this, [this]()
    {
        // Zero: release everything still waiting. A stalled voice must not cost the user
        // the reply itself.
        ReleasePendingSpeechText(0);
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
    if (capabilityWorker.joinable())
    {
        capabilityWorker.join();
    }
    session.Stop();
    if (voiceWorker.joinable())
    {
        voiceWorker.join();
    }
}

void ReviaWindow::RequestShutdown(const revia::core::ExitReason reason)
{
    BeginShutdown(reason);
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
    // No tray icon to retreat to, so this close really does end the session.
    revia::core::ExitReporter::Record(
        revia::core::ExitReason::WindowClosed,
        "the window was closed with no tray icon available");
    event->accept();
}

void ReviaWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    ApplyContentWidthCap();
}

void ReviaWindow::ApplyContentWidthCap()
{
    if (ui == nullptr || ui->rootLayout == nullptr)
    {
        return;
    }
    // Text stops being easier to read somewhere around this width and starts being
    // harder: the eye loses its place returning to the start of the next line, and a
    // six-item status row spreads into six islands with nothing between them. Past the
    // cap the extra pixels become margin instead of content, so a maximised window shows
    // the same layout as a windowed one rather than a stretched relative of it.
    // Scales with the window instead of stopping dead at a fixed measure.
    //
    // A hard 1500px cap meant maximising on a wide monitor changed nothing except the
    // size of the empty margins, which is not what maximising is for. The cap now grows
    // with the window: 1500px is a floor for small windows so a narrow one still gets
    // sensible margins, and beyond that content takes most of the width while keeping a
    // margin so nothing sits against the frame.
    constexpr int readableFloor = 1500;
    constexpr int minimumSideMargin = 22;
    const int maximumContentWidth = std::max(
        readableFloor, static_cast<int>(static_cast<double>(width()) * 0.9));
    const int side = std::max(
        minimumSideMargin, (width() - maximumContentWidth) / 2);
    ui->rootLayout->setContentsMargins(side, 20, side, 20);
}

void ReviaWindow::BuildInterface()
{
    ui->setupUi(this);

    titleBar = ui->titleBar;
    maximizeButton = ui->maximizeButton;
    stateLabel = ui->stateLabel;
    stateDetailLabel = ui->stateDetailLabel;
    affectLabel = ui->affectLabel;
    speechLabel = ui->speechLabel;
    microphoneLabel = ui->microphoneLabel;
    automationLabel = ui->automationLabel;
    visionLabel = ui->visionLabel;
    perceptionLabel = ui->perceptionLabel;
    tabs = ui->tabs;
    chatHistory = ui->chatHistory;
    messageInput = ui->messageInput;
    activityFeed = ui->activityFeed;
    activityIssueSummary = ui->activityIssueSummary;
    activityFilter = ui->activityFilter;
    activityAutoScroll = ui->activityAutoScroll;
    openLogsButton = ui->openLogsButton;
    clearActivityButton = ui->clearActivityButton;
    sendButton = ui->sendButton;
    stopButton = ui->stopButton;
    microphoneButton = ui->microphoneButton;
    screenActionButton = ui->screenActionButton;
    microphoneDeviceCombo = ui->microphoneDeviceCombo;
    refreshMicrophonesButton = ui->refreshMicrophonesButton;
    testMicrophoneButton = ui->testMicrophoneButton;
    microphoneTestResultLabel = ui->microphoneTestResultLabel;
    alwaysOnTopCheck = ui->alwaysOnTopCheck;
    speechCheck = ui->speechCheck;
    autoSendVoiceCheck = ui->autoSendVoiceCheck;
    bargeInCheck = ui->bargeInCheck;
    handsFreeCheck = ui->handsFreeCheck;
    avatarBridgeCheck = ui->avatarBridgeCheck;
    externalAdaptersCheck = ui->externalAdaptersCheck;
    initiativeCheck = ui->initiativeCheck;
    curiosityCheck = ui->curiosityCheck;
    spontaneousSpeechCheck = ui->spontaneousSpeechCheck;
    speakWhenAwayCheck = ui->speakWhenAwayCheck;
    aiFilterCheck = ui->aiFilterCheck;
    initiativeMaxSpin = ui->initiativeMaxSpin;
    resourceSampleSpin = ui->resourceSampleSpin;
    preferenceStatus = ui->preferenceStatus;
    presencePhaseValue = ui->presencePhaseValue;
    presenceAffectValue = ui->presenceAffectValue;
    presenceAttentionValue = ui->presenceAttentionValue;
    presenceMomentumBar = ui->presenceMomentumBar;
    openPresenceFolderButton = ui->openPresenceFolderButton;
    voiceLibraryCombo = ui->voiceLibraryCombo;
    voiceLanguageCombo = ui->voiceLanguageCombo;
    voiceNameInput = ui->voiceNameInput;
    voiceDescriptionInput = ui->voiceDescriptionInput;
    voiceReferenceInput = ui->voiceReferenceInput;
    voicePreviewInput = ui->voicePreviewInput;
    voiceStudioStatus = ui->voiceStudioStatus;
    createVoiceButton = ui->createVoiceButton;
    previewVoiceButton = ui->previewVoiceButton;

    ui->titleIcon->setPixmap(CreateReviaIcon().pixmap(20, 20));
    titleBar->installEventFilter(this);
    messageInput->installEventFilter(this);

    pipelinePanel = new PipelinePanel(ui->pipelinePage);
    ui->pipelineHostLayout->addWidget(pipelinePanel);
    internetActivityPanel = new InternetActivityPanel(ui->internetActivityPage);
    ui->internetActivityHostLayout->addWidget(internetActivityPanel);
    resourcePanel = new ResourcePanel(
        [this](const std::string& device)
        {
            return session.SetPreference("resources.voiceDevice", device);
        },
        ui->resourcePage);
    ui->resourceHostLayout->addWidget(resourcePanel);
    canvasPanel = new CanvasPanel(ui->canvasPage);
    ui->canvasHostLayout->addWidget(canvasPanel);
    capabilityPanel = new CapabilityPanel(
        session,
        [this]() { DiscoverApplicationPermissions(); },
        ui->permissionsPage);
    ui->permissionsHostLayout->addWidget(capabilityPanel);
    profilePanel = new ProfilePanel(session, ui->profilesPage);
    ui->profilesHostLayout->addWidget(profilePanel);
    memoryPanel = new MemoryPanel(session, ui->memoryTab);
    ui->memoryHostLayout->addWidget(memoryPanel);
    mindPanel = new MindPanel(session, ui->mindTab);
    ui->mindHostLayout->addWidget(mindPanel);
    visionPanel = new VisionPanel(session, ui->visionTab);
    ui->visionHostLayout->addWidget(visionPanel);

    voiceLanguageCombo->addItems({"English", "Chinese", "Japanese", "Korean", "German",
        "French", "Russian", "Portuguese", "Spanish", "Italian"});

    connect(ui->minimizeButton, &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(maximizeButton, &QToolButton::clicked, this, [this]() { ToggleMaximized(); });
    connect(ui->closeButton, &QToolButton::clicked, this, &QWidget::close);

    connect(chatHistory, &QTextBrowser::anchorClicked, this, [this](const QUrl& link)
    {
        const QString target = link.toString();
        if (!target.startsWith(QStringLiteral("thought:")))
        {
            return;
        }
        bool parsed = false;
        const std::size_t index =
            static_cast<std::size_t>(target.mid(8).toULongLong(&parsed));
        if (parsed && index < chatEntries.size())
        {
            chatEntries[index].expanded = !chatEntries[index].expanded;
            RenderChat();
        }
    });

    QSettings shellSettings;
    const int savedActivityFilter = std::clamp(
        shellSettings.value("activity/filter", 0).toInt(), 0, activityFilter->count() - 1);
    activityFilter->setCurrentIndex(savedActivityFilter);
    activityAutoScroll->setChecked(
        shellSettings.value("activity/follow", true).toBool());
    connect(activityFilter, &QComboBox::currentIndexChanged, this, [this](const int index)
    {
        QSettings().setValue("activity/filter", index);
        RenderActivity();
    });
    connect(activityAutoScroll, &QCheckBox::toggled, this, [](const bool enabled)
    {
        QSettings().setValue("activity/follow", enabled);
    });
    connect(clearActivityButton, &QPushButton::clicked, this, [this]()
    {
        activityEntries.clear();
        activityWarningCount = 0;
        activityErrorCount = 0;
        RenderActivity();
        UpdateActivitySummary();
    });
    connect(openLogsButton, &QPushButton::clicked, this, [this]()
    {
        const QString path = QDir::current().filePath(QStringLiteral("Logs"));
        QDir().mkpath(path);
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        {
            AppendActivity(
                QStringLiteral("The Logs folder could not be opened: ") + path,
                ActivitySeverity::Warning);
        }
    });
    UpdateActivitySummary();

    connect(sendButton, &QPushButton::clicked, this, [this]() { SendMessage(); });
    connect(stopButton, &QPushButton::clicked, this, [this]() { session.RequestStop(); });
    connect(microphoneButton, &QPushButton::clicked, this, [this]() { ToggleListening(); });
    connect(screenActionButton, &QPushButton::clicked, this, [this]() { UseVisibleScreen(); });
    RefreshMicrophoneDevices();
    connect(refreshMicrophonesButton, &QPushButton::clicked, this, [this]()
    {
        RefreshMicrophoneDevices();
    });
    connect(testMicrophoneButton, &QPushButton::clicked, this, [this]()
    {
        RunMicrophoneTest();
    });
    connect(microphoneDeviceCombo, &QComboBox::currentIndexChanged, this,
        [this](const int index)
        {
            if (index < 0 || microphoneDeviceCombo->signalsBlocked())
            {
                return;
            }
            const QString device =
                microphoneDeviceCombo->itemData(index).toString();
            QSettings().setValue("input/microphoneDevice", device);
            session.SetMicrophoneDevice(device.toStdString());
        });
    alwaysOnTopCheck->setChecked(shellSettings.value("window/alwaysOnTop", false).toBool());
    autoSendVoiceCheck->setChecked(shellSettings.value("input/autoSendVoice", true).toBool());
    connect(alwaysOnTopCheck, &QCheckBox::toggled, this, [this](const bool enabled)
    {
        QSettings().setValue("window/alwaysOnTop", enabled);
        SetAlwaysOnTop(enabled);
    });
    connect(autoSendVoiceCheck, &QCheckBox::toggled, this, [](const bool enabled)
    {
        QSettings().setValue("input/autoSendVoice", enabled);
    });
    if (alwaysOnTopCheck->isChecked())
    {
        SetAlwaysOnTop(true);
    }
    connect(speechCheck, &QCheckBox::toggled, this, [this](const bool enabled)
    {
        ShowPreferenceResult(
            session.SetPreference("speech.enabled", enabled ? "on" : "off"));
    });
    connect(bargeInCheck, &QCheckBox::toggled, this, [this](const bool enabled)
    {
        ShowPreferenceResult(
            session.SetPreference("bargeIn.enabled", enabled ? "on" : "off"));
    });
    connect(handsFreeCheck, &QCheckBox::toggled, this, [this](const bool enabled)
    {
        ShowPreferenceResult(session.SetPreference(
            "speechRecognition.handsFree", enabled ? "on" : "off"));
        autoSendVoiceCheck->setEnabled(!enabled);
    });
    connect(avatarBridgeCheck, &QCheckBox::toggled, this, [this](const bool enabled)
    {
        ShowPreferenceResult(session.SetPreference(
            "presence.avatarBridgeEnabled", enabled ? "on" : "off"));
    });
    connect(externalAdaptersCheck, &QCheckBox::toggled, this, [this](const bool enabled)
    {
        ShowPreferenceResult(session.SetPreference(
            "presence.externalAdaptersEnabled", enabled ? "on" : "off"));
    });
    connect(openPresenceFolderButton, &QPushButton::clicked, this, [this]()
    {
        const QString path = QDir::current().filePath(QStringLiteral("RuntimeData/Presence"));
        QDir().mkpath(path);
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        {
            AppendActivity(
                QStringLiteral("The Presence folder could not be opened: ") + path,
                ActivitySeverity::Warning);
        }
    });
    connect(initiativeCheck, &QCheckBox::toggled, this, [this](const bool enabled)
    {
        initiativeMaxSpin->setEnabled(enabled);
        curiosityCheck->setEnabled(enabled);
        spontaneousSpeechCheck->setEnabled(enabled && curiosityCheck->isChecked());
        speakWhenAwayCheck->setEnabled(enabled && curiosityCheck->isChecked());
        ShowPreferenceResult(
            session.SetPreference("initiative.enabled", enabled ? "on" : "off"));
    });
    connect(curiosityCheck, &QCheckBox::toggled, this, [this](const bool enabled)
    {
        spontaneousSpeechCheck->setEnabled(initiativeCheck->isChecked() && enabled);
        speakWhenAwayCheck->setEnabled(initiativeCheck->isChecked() && enabled);
        ShowPreferenceResult(session.SetPreference(
            "initiative.curiosityEnabled", enabled ? "on" : "off"));
    });
    connect(spontaneousSpeechCheck, &QCheckBox::toggled, this, [this](const bool enabled)
    {
        ShowPreferenceResult(session.SetPreference(
            "initiative.spontaneousSpeechEnabled", enabled ? "on" : "off"));
    });
    connect(speakWhenAwayCheck, &QCheckBox::toggled, this, [this](const bool enabled)
    {
        ShowPreferenceResult(session.SetPreference(
            "initiative.speakWhenUserAway", enabled ? "on" : "off"));
    });
    connect(aiFilterCheck, &QCheckBox::toggled, this, [this](const bool enabled)
    {
        ShowPreferenceResult(session.SetPreference(
            "responseFilter.aiReviewEnabled", enabled ? "on" : "off"));
    });
    connect(initiativeMaxSpin, &QSpinBox::editingFinished, this, [this]()
    {
        ShowPreferenceResult(session.SetPreference(
            "initiative.maxPerHour", std::to_string(initiativeMaxSpin->value())));
    });
    connect(resourceSampleSpin, &QSpinBox::editingFinished, this, [this]()
    {
        ShowPreferenceResult(session.SetPreference(
            "resources.usageSampleSeconds", std::to_string(resourceSampleSpin->value())));
    });
    connect(createVoiceButton, &QPushButton::clicked, this, [this]() { CreateVoicePreset(); });
    connect(previewVoiceButton, &QPushButton::clicked, this, [this]() { PreviewVoice(); });
    RefreshVoiceStudio();

    QFile theme(":/revia/revia.qss");
    if (theme.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        setStyleSheet(QString::fromUtf8(theme.readAll()));
    }
    else
    {
        AppendActivity("The embedded Revia theme could not be loaded.");
    }

    ApplyMicrophoneUi(MicrophoneUi::Unavailable);
    ApplyContentWidthCap();
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
    alwaysOnTopAction->setChecked(alwaysOnTopCheck->isChecked());
    connect(quitAction, &QAction::triggered, this, [this]()
    {
        BeginShutdown(revia::core::ExitReason::TrayQuit);
    });
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
            screenActionButton->setEnabled(session.IsVisionAvailable());
            stopButton->setEnabled(false);
            // Re-evaluate the microphone now that IsStarted() is finally true; the last
            // recognition phase event arrived while startup was still in flight.
            ApplyMicrophoneUi(microphoneUiState);
            if (!greeting.isEmpty())
            {
                AppendChat(QString::fromStdString(session.DisplayName()), greeting);
            }
            if (!started)
            {
                AppendActivity(
                    "Startup did not complete. Check the activity log.",
                    ActivitySeverity::Error);
            }
            else
            {
                ApplyUserPreferences();
            }
            RefreshVoiceStudio();
            if (resourcePanel != nullptr && started)
            {
                resourcePanel->SetVoiceDevicePreference(session.VoiceDevicePreference());
            }
            if (capabilityPanel != nullptr)
            {
                capabilityPanel->Refresh();
            }
            if (memoryPanel != nullptr)
            {
                memoryPanel->Refresh();
            }
            if (mindPanel != nullptr)
            {
                mindPanel->Refresh();
            }
            if (visionPanel != nullptr)
            {
                visionPanel->Refresh();
            }
            messageInput->setFocus();
        }, Qt::QueuedConnection);
    });
}

void ReviaWindow::SendMessage(const bool voiceInput)
{
    // Voice may arrive while Revia is still replying; that is what the merge window is
    // for. Typed input still waits, because pressing Enter mid-reply and having it
    // silently queue would be surprising.
    if (shuttingDown.load() || (!voiceInput && session.IsBusy()))
    {
        return;
    }

    const QString text = messageInput->toPlainText().trimmed();
    if (text.isEmpty())
    {
        return;
    }
    messageInput->clear();

    if (voiceInput)
    {
        // Offered, not submitted. It merges with anything else said in the next moment,
        // and the reply arrives as an event once that window closes.
        const revia::agents::InputVerdict verdict =
            session.OfferInput(text.toStdString(), revia::agents::InputSource::Voice);
        if (verdict == revia::agents::InputVerdict::Queued)
        {
            AppendChat("You", text, true);
        }
        else
        {
            // Shown rather than dropped in silence, so an over-eager filter is visible.
            AppendActivity(QStringLiteral("Heard but not answered: \"") + text +
                QStringLiteral("\" - ") +
                QString::fromStdString(revia::agents::ToString(verdict)));
        }
        messageInput->setFocus();
        return;
    }

    AppendChat("You", text, true);
    sendButton->setEnabled(false);
    stopButton->setEnabled(true);

    if (operationWorker.joinable())
    {
        operationWorker.join();
    }
    operationWorker = std::jthread([this, input = text.toStdString(), voiceInput]()
    {
        const revia::runtime::SessionResult result = session.Submit(
            input,
            voiceInput
                ? revia::agents::InputSource::Voice
                : revia::agents::InputSource::Typed);
        QMetaObject::invokeMethod(this, [this, result]()
        {
            const QString reasoning = QString::fromStdString(result.reasoning);
            // Already shown sentence by sentence while it was being spoken.
            if (!result.text.empty() && !result.spokenAsFragments)
            {
                const QString speaker = result.fromAssistant
                    ? QString::fromStdString(session.DisplayName())
                    : QStringLiteral("System");
                const QString body = QString::fromStdString(result.text);
                if (result.speechPending && result.utteranceId != 0)
                {
                    // Hold it. The Speaking event for this utterance releases it, so the
                    // words appear as Revia starts saying them.
                    if (lastSpeakingUtteranceId == result.utteranceId)
                    {
                        // Audio already started while this was being handed over.
                        AppendChat(speaker, body, false, reasoning);
                    }
                    else
                    {
                        pendingUtterances[result.utteranceId] = {speaker, body, reasoning};
                        pendingSpeechTimer->start();
                    }
                }
                else
                {
                    AppendChat(speaker, body, false, reasoning);
                }
            }
            else if (result.spokenAsFragments && !reasoning.isEmpty())
            {
                // The reply was shown a sentence at a time, so the trace for the turn as a
                // whole gets its own collapsed line rather than being attached to whichever
                // fragment happened to be last.
                AppendChat(QString(), QString(), false, reasoning);
            }
            if (!result.succeeded && !result.reason.empty())
            {
                AppendActivity("Request stopped: " + QString::fromStdString(result.reason));
            }
            sendButton->setEnabled(!result.shouldExit && session.IsStarted());
            stopButton->setEnabled(false);
            if (result.shouldExit)
            {
                BeginShutdown(revia::core::ExitReason::UserCommand,
                    "the conversation asked Revia to exit");
            }
            else
            {
                messageInput->setFocus();
            }
        }, Qt::QueuedConnection);
    });
}

void ReviaWindow::ReleasePendingSpeechText(const std::uint64_t utteranceId)
{
    if (pendingUtterances.empty())
    {
        return;
    }
    if (utteranceId == 0)
    {
        // Everything waiting, oldest first. Used by the fallback timer and by any speech
        // failure, because text that is never shown is the worst outcome available.
        auto waiting = std::move(pendingUtterances);
        pendingUtterances.clear();
        pendingSpeechTimer->stop();
        for (const auto& entry : waiting)
        {
            AppendChat(entry.second.speaker, entry.second.text, false,
                entry.second.reasoning);
        }
        return;
    }

    const auto found = pendingUtterances.find(utteranceId);
    if (found == pendingUtterances.end())
    {
        return;
    }
    // Anything queued before this one has been overtaken; show it first so the order of
    // the conversation is never scrambled by a dropped utterance.
    for (auto entry = pendingUtterances.begin(); entry != found;)
    {
        AppendChat(entry->second.speaker, entry->second.text, false,
            entry->second.reasoning);
        entry = pendingUtterances.erase(entry);
    }
    AppendChat(found->second.speaker, found->second.text, false, found->second.reasoning);
    pendingUtterances.erase(found);
    if (pendingUtterances.empty())
    {
        pendingSpeechTimer->stop();
    }
}

void ReviaWindow::RefreshMicrophoneDevices()
{
    const QSignalBlocker blocker(microphoneDeviceCombo);
    const QString saved =
        QSettings().value("input/microphoneDevice", QString()).toString();
    microphoneDeviceCombo->clear();
    microphoneDeviceCombo->addItem(QStringLiteral("Default"), QString());
    for (const revia::speech::MicrophoneDevice& device : session.AvailableMicrophones())
    {
        const QString name = QString::fromStdString(device.name);
        microphoneDeviceCombo->addItem(name, name);
    }

    // The saved device is offered even when it is not present, so the selection the
    // user made is still visible rather than silently reverting to Default and looking
    // like it was never set. Capture falls back at open time and reports it.
    int index = microphoneDeviceCombo->findData(saved);
    if (index < 0 && !saved.isEmpty())
    {
        microphoneDeviceCombo->addItem(saved + QStringLiteral(" (not connected)"), saved);
        index = microphoneDeviceCombo->count() - 1;
    }
    microphoneDeviceCombo->setCurrentIndex(index < 0 ? 0 : index);
    session.SetMicrophoneDevice(saved.toStdString());

    const revia::speech::MicrophoneSelection resolved = session.ResolvedMicrophone();
    microphoneDeviceCombo->setToolTip(QString::fromStdString(resolved.report));
    if (resolved.fellBackToDefault)
    {
        microphoneTestResultLabel->setText(
            QStringLiteral("Microphone error: ") +
            QString::fromStdString(resolved.report));
        AppendActivity(QStringLiteral("Microphone: ") +
            QString::fromStdString(resolved.report));
    }
}

void ReviaWindow::RunMicrophoneTest()
{
    if (shuttingDown.load())
    {
        return;
    }
    testMicrophoneButton->setEnabled(false);
    microphoneTestResultLabel->setText(QStringLiteral("Testing microphone..."));
    // Synchronous and short. Running it on the UI thread keeps the device lifetime
    // trivially correct, and the button is disabled for the few seconds it takes.
    QApplication::processEvents();
    const revia::speech::MicrophoneTestResult result = session.TestMicrophone(3, true);
    const QString status = QString::fromStdString(result.status);
    const QString message = QString::fromStdString(result.message);
    microphoneTestResultLabel->setText(
        (result.succeeded ? QStringLiteral("Microphone OK - ")
                          : QStringLiteral("Microphone error: ")) + message);
    AppendActivity(QStringLiteral("Microphone test (") + status +
        QStringLiteral("): ") + message);
    testMicrophoneButton->setEnabled(true);
}

void ReviaWindow::ToggleListening()
{
    if (shuttingDown.load())
    {
        return;
    }

    if (microphoneUiState == MicrophoneUi::Listening)
    {
        listenRequested = false;
        // EndListening hands the capture to whisper.cpp; the Transcribing phase event
        // arrives from the worker and repaints the button.
        ApplyMicrophoneUi(session.EndListening()
            ? MicrophoneUi::Transcribing
            : MicrophoneUi::Ready);
        return;
    }

    if (microphoneUiState != MicrophoneUi::Ready)
    {
        return;
    }
    if (session.BeginListening())
    {
        listenRequested = true;
        ApplyMicrophoneUi(MicrophoneUi::Listening);
    }
    else
    {
        // The reason itself arrives as a Microphone/Error event from the session, which
        // puts it in the label and the status bar. This line only guarantees the
        // activity feed records the press, so a press that produced nothing is still
        // visible in the transcript of what happened.
        AppendActivity("Microphone: listening could not start. See the microphone "
            "status for the reason.");
    }
}

void ReviaWindow::ApplyMicrophoneUi(const MicrophoneUi microphoneUi)
{
    microphoneUiState = microphoneUi;
    microphoneActive = microphoneUi == MicrophoneUi::Listening ||
        microphoneUi == MicrophoneUi::Transcribing;

    switch (microphoneUi)
    {
        case MicrophoneUi::HandsFree:
            microphoneButton->setText("Hands-free on");
            microphoneButton->setToolTip(
                "Voice activity detection is waiting for a complete spoken thought. "
                "Turn it off from the Presence tab to use push-to-talk.");
            microphoneButton->setEnabled(false);
            break;
        case MicrophoneUi::Listening:
            microphoneButton->setText("Stop listening");
            microphoneButton->setToolTip("Stop listening and transcribe (Ctrl+Space)");
            microphoneButton->setEnabled(true);
            break;
        case MicrophoneUi::Transcribing:
            microphoneButton->setText("Transcribing...");
            microphoneButton->setToolTip("Whisper is turning the recording into text.");
            microphoneButton->setEnabled(false);
            break;
        case MicrophoneUi::Ready:
            microphoneButton->setText("Listen");
            // Recognition reports Ready partway through startup, while the startup thread
            // is still running. Taking a turn then would block the UI joining it, so the
            // button stays disabled until the runtime is actually up.
            microphoneButton->setToolTip(session.IsStarted()
                ? "Start listening (Ctrl+Space)"
                : "Available once Revia has finished starting.");
            microphoneButton->setEnabled(session.IsStarted());
            break;
        case MicrophoneUi::Unavailable:
        default:
            microphoneButton->setText("Listen");
            microphoneButton->setToolTip("Speech recognition is not available.");
            microphoneButton->setEnabled(false);
            break;
    }

    // The pressed-state styling is only meaningful while a press is held, so recolour
    // the button from its logical state instead.
    microphoneButton->setProperty("listening", microphoneUi == MicrophoneUi::Listening);
    microphoneButton->style()->unpolish(microphoneButton);
    microphoneButton->style()->polish(microphoneButton);

    stopButton->setEnabled(microphoneActive || speechActive || session.IsBusy());
}

void ReviaWindow::BeginShutdown(
    const revia::core::ExitReason reason,
    const std::string& detail)
{
    if (shuttingDown.exchange(true))
    {
        return;
    }
    // Recorded here rather than after the workers stop, so the reason survives even if
    // shutdown itself is what fails.
    revia::core::ExitReporter::Record(reason, detail);
    sendButton->setEnabled(false);
    stopButton->setEnabled(false);
    session.RequestStop();
    shutdownWorker = std::jthread([this]()
    {
        session.Stop();
        QMetaObject::invokeMethod(qApp, []() { QApplication::quit(); }, Qt::QueuedConnection);
    });
}

void ReviaWindow::UseVisibleScreen()
{
    if (shuttingDown.load() || session.IsBusy())
    {
        return;
    }
    const QString instruction = messageInput->toPlainText().trimmed();
    if (instruction.isEmpty())
    {
        QMessageBox::information(
            this,
            "Screen action instruction",
            "Write one specific instruction first, such as 'press the Save button'.");
        messageInput->setFocus();
        return;
    }
    const bool approved = QMessageBox::question(
        this,
        "Share and resolve screen control",
        "Revia will capture only the foreground application window once, locate the requested control "
        "locally, and require a match to a real Windows UI Automation element. The "
        "temporary PNG is deleted before any action. There is no coordinate-click "
        "fallback, and the resulting action still requires normal policy approval. "
        "Continue?",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No) == QMessageBox::Yes;
    if (!approved)
    {
        return;
    }

    messageInput->clear();
    AppendChat("You", instruction, true);
    showMinimized();
    sendButton->setEnabled(false);
    screenActionButton->setEnabled(false);
    stopButton->setEnabled(true);
    if (operationWorker.joinable())
    {
        operationWorker.join();
    }
    operationWorker = std::jthread([this, request = instruction.toStdString()]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        const revia::runtime::SessionResult result = session.ActOnScreen(request);
        QMetaObject::invokeMethod(this, [this, result]()
        {
            showNormal();
            raise();
            activateWindow();
            if (!result.text.empty())
            {
                AppendChat(
                    QString::fromStdString(session.DisplayName()),
                    QString::fromStdString(result.text));
            }
            if (!result.succeeded && !result.reason.empty())
            {
                AppendActivity(
                    "Screen action stopped: " + QString::fromStdString(result.reason));
            }
            sendButton->setEnabled(session.IsStarted());
            screenActionButton->setEnabled(session.IsVisionAvailable());
            stopButton->setEnabled(false);
            messageInput->setFocus();
        }, Qt::QueuedConnection);
    });
}

void ReviaWindow::DiscoverApplicationPermissions()
{
    if (shuttingDown.load() || session.IsBusy())
    {
        if (capabilityPanel != nullptr)
        {
            capabilityPanel->SetStatus(
                "Wait for the current Revia operation to finish before discovery.", true);
        }
        return;
    }
    showMinimized();
    if (capabilityWorker.joinable())
    {
        capabilityWorker.join();
    }
    capabilityWorker = std::jthread([this]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(450));
        const auto inventory = session.DiscoverForegroundApplicationControls();
        QMetaObject::invokeMethod(this, [this, inventory]()
        {
            showNormal();
            raise();
            activateWindow();
            if (capabilityPanel != nullptr)
            {
                capabilityPanel->ShowDiscovery(inventory);
            }
        }, Qt::QueuedConnection);
    });
}

void ReviaWindow::RefreshVoiceStudio()
{
    const revia::speech::VoiceStudioSnapshot snapshot = session.VoiceStudio();
    {
        const QSignalBlocker libraryBlocker(voiceLibraryCombo);
        const QString selected = voiceLibraryCombo->currentData().toString();
        voiceLibraryCombo->clear();
        voiceLibraryCombo->addItem("Select a created voice...", QString());
        for (const auto& preset : snapshot.presets)
        {
            voiceLibraryCombo->addItem(
                QString::fromStdString(preset.name),
                QString::fromStdString(preset.id));
        }
        // Prefer what the user was looking at; otherwise show the voice the running
        // profile actually speaks with, so Preview answers "what do I sound like now".
        QString target = selected;
        if (target.isEmpty())
        {
            target = QString::fromStdString(snapshot.assignedPresetId);
        }
        const int index = voiceLibraryCombo->findData(target);
        voiceLibraryCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (!voiceOperationRunning.load())
    {
        voiceStudioStatus->setText(snapshot.presets.empty()
            ? "No voices have been created yet. Describe one below and create it."
            : QString::number(static_cast<int>(snapshot.presets.size())) +
                (snapshot.presets.size() == 1
                    ? " voice is available. Assign it to a profile under Profiles."
                    : " voices are available. Assign one to a profile under Profiles."));
    }
    if (profilePanel != nullptr)
    {
        profilePanel->Refresh();
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
    for (QPushButton* button : {createVoiceButton, previewVoiceButton})
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
            for (QPushButton* button : {createVoiceButton, previewVoiceButton})
            {
                button->setEnabled(true);
            }
            RefreshVoiceStudio();
            const int createdIndex = voiceLibraryCombo->findText(name);
            if (createdIndex >= 0)
            {
                voiceLibraryCombo->setCurrentIndex(createdIndex);
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
    const QString presetId = voiceLibraryCombo->currentData().toString();
    const QString preview = voicePreviewInput->toPlainText().trimmed();
    if (presetId.isEmpty() || preview.isEmpty())
    {
        voiceOperationRunning.store(false);
        voiceStudioStatus->setText("Select a created voice and enter a preview line first.");
        return;
    }
    for (QPushButton* button : {createVoiceButton, previewVoiceButton})
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
            for (QPushButton* button : {createVoiceButton, previewVoiceButton})
            {
                button->setEnabled(true);
            }
            voiceStudioStatus->setText(QString::fromStdString(result.message));
        }, Qt::QueuedConnection);
    });
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
    const bool wasVisible = isVisible();
    const QRect previousGeometry = geometry();
    setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
    if (wasVisible)
    {
        show();
        setGeometry(previousGeometry);
    }
}

void ReviaWindow::HandleRuntimeEvent(const revia::runtime::RuntimeEvent& event)
{
    RefreshPresenceUi();
    if (pipelinePanel != nullptr)
    {
        pipelinePanel->Observe(event);
    }
    if (resourcePanel != nullptr)
    {
        resourcePanel->Observe(event);
    }
    if (internetActivityPanel != nullptr)
    {
        internetActivityPanel->Observe(event);
    }
    if (canvasPanel != nullptr)
    {
        canvasPanel->Observe(event);
    }
    if (event.kind == revia::runtime::RuntimeEventKind::UserMessage)
    {
        const bool adapter = event.component == "Adapters";
        AppendChat(adapter ? QStringLiteral("External") : QStringLiteral("You"),
            QString::fromStdString(event.message), true);
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::StateChanged)
    {
        UpdateState(event.state, QString::fromStdString(event.message));
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::ReplyFragment)
    {
        // Handed to speech, not yet spoken. Speak() only queues, and with Qwen the audio
        // begins seconds later, so this waits for its own Speaking event.
        const QString speaker = QString::fromStdString(session.DisplayName());
        const QString body = QString::fromStdString(event.message);
        if (event.turnId != 0 && event.turnId != lastSpeakingUtteranceId)
        {
            pendingUtterances[event.turnId] = {speaker, body};
            pendingSpeechTimer->start();
        }
        else
        {
            AppendChat(speaker, body);
        }
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::Proposal)
    {
        // Revia speaking first. Shown with its evidence and dismissible in one action,
        // which is the whole contract: it offers, it never acts.
        const QString message = QString::fromStdString(event.message);
        AppendChat(QString::fromStdString(session.DisplayName()), message);
        AppendActivity(QStringLiteral("Proposal ") + QString::fromStdString(event.phase) +
            QStringLiteral(" because ") + QString::fromStdString(event.detail) +
            QStringLiteral(" - /initiative accept or /initiative dismiss"));
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::SelfInquiry)
    {
        // Shown in the transcript rather than collapsed behind "Thought process". These
        // are the questions Revia put to herself on a hard turn, and the point of them is
        // that the user sees what she was actually wondering before the answer arrives.
        AppendChat(
            QString::fromStdString(session.DisplayName()) + QStringLiteral(" is thinking"),
            QString::fromStdString(event.message));
        AppendActivity(QStringLiteral("Self-inquiry: ") +
            QString::fromStdString(event.detail));
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
                phase == QStringLiteral("Generating") || phase == QStringLiteral("Generated") ||
                phase == QStringLiteral("Speaking");

            speechPhase = phase;
            RefreshStateBadge();
            if (phase == QStringLiteral("Speaking"))
            {
                // The sync point: audio has started, so show the words now.
                lastSpeakingUtteranceId = event.turnId;
                ReleasePendingSpeechText(event.turnId);
            }
            else if (phase == QStringLiteral("Error") ||
                phase == QStringLiteral("Unavailable") ||
                phase == QStringLiteral("Disabled") ||
                phase == QStringLiteral("Stopped") ||
                phase == QStringLiteral("Interrupted"))
            {
                // Speech is not coming, or has ended early. Never leave the reply unshown
                // because the audio failed.
                ReleasePendingSpeechText(0);
            }
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
            if (phase == QStringLiteral("Missing") || phase == QStringLiteral("Disabled") ||
                phase == QStringLiteral("Unavailable"))
            {
                listenRequested = false;
                ApplyMicrophoneUi(MicrophoneUi::Unavailable);
            }
            else if (phase == QStringLiteral("HandsFree"))
            {
                listenRequested = false;
                ApplyMicrophoneUi(MicrophoneUi::HandsFree);
            }
            else if (phase == QStringLiteral("SpeechDetected"))
            {
                ApplyMicrophoneUi(MicrophoneUi::Listening);
            }
            else if (phase == QStringLiteral("Recording"))
            {
                // The capture thread confirms it opened the microphone. Ignore it if the
                // user already toggled off, so a late event cannot revive the button.
                if (listenRequested)
                {
                    ApplyMicrophoneUi(MicrophoneUi::Listening);
                }
            }
            else if (phase == QStringLiteral("Captured") ||
                phase == QStringLiteral("Transcribing"))
            {
                ApplyMicrophoneUi(MicrophoneUi::Transcribing);
            }
            else if (phase == QStringLiteral("Diagnostics") ||
                phase.startsWith(QStringLiteral("Test")))
            {
                // Neither a recording nor the end of one. Diagnostics reports what a
                // capture measured, and the Test* phases belong to the microphone test,
                // which deliberately runs outside the Listen cycle. Letting either fall
                // into the branch below would end a cycle that is still going.
            }
            else
            {
                // Ready, Transcript, Stopped, and Error all end the cycle. Error is
                // recoverable here on purpose: a microphone that failed to open once
                // should not lock the button for the rest of the session.
                listenRequested = false;
                ApplyMicrophoneUi(MicrophoneUi::Ready);
            }
            if (phase == QStringLiteral("Error"))
            {
                // Said where the user is looking, not only in the activity feed. The
                // button returning to "Listen" with no other change is what made a
                // microphone that could not open look like a button that did nothing.
                const QString reason = QString::fromStdString(event.message);
                microphoneTestResultLabel->setText(reason);
                microphoneLabel->setText(QStringLiteral("Mic: error"));
                statusBar()->showMessage(reason, 12000);
            }
            else if (phase == QStringLiteral("Recording"))
            {
                microphoneTestResultLabel->clear();
            }
            if (phase == QStringLiteral("Transcript"))
            {
                const QString transcript = QString::fromStdString(event.message).trimmed();
                using revia::speech::TranscriptRouting;
                switch (revia::speech::DecideTranscriptRouting(
                    event.detail == "hands-free",
                    transcript.isEmpty(),
                    autoSendVoiceCheck->isChecked(),
                    session.IsBusy()))
                {
                    case TranscriptRouting::Ignore:
                        // Deliberately leaves the message box alone: an empty
                        // transcript must not erase something already typed there.
                        AppendActivity(QStringLiteral(
                            "Microphone: nothing was recognised in that recording."));
                        break;
                    case TranscriptRouting::HandsFreeAlreadySubmitted:
                        ApplyMicrophoneUi(MicrophoneUi::HandsFree);
                        break;
                    case TranscriptRouting::FillAndSend:
                        messageInput->setPlainText(transcript);
                        SendMessage(true);
                        break;
                    case TranscriptRouting::FillAndHold:
                        messageInput->setPlainText(transcript);
                        messageInput->setFocus();
                        break;
                }
            }
            QString detail = QStringLiteral("Microphone ") + phase + QStringLiteral(": ") +
                QString::fromStdString(event.message);
            if (event.elapsedMilliseconds >= 0.0)
            {
                detail += QStringLiteral(" (") +
                    QString::number(event.elapsedMilliseconds, 'f', 1) + QStringLiteral("ms)");
            }
            // ApplyMicrophoneUi already refreshed the stop button, and it ran before the
            // transcript may have started a turn, so do not overwrite that here.
            AppendActivity(detail);
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
        else if (event.component == "Perception")
        {
            const QString phase = QString::fromStdString(event.phase);
            const QString detail = QString::fromStdString(event.message);
            const bool lifecycle = phase == QStringLiteral("Watching") ||
                phase == QStringLiteral("Paused") || phase == QStringLiteral("Off") ||
                phase == QStringLiteral("Error") || phase == QStringLiteral("Unavailable");
            if (lifecycle)
            {
                perceptionLabel->setText(QStringLiteral("Watching: ") +
                    (phase == QStringLiteral("Watching") ? QStringLiteral("On") : phase));
                perceptionLabel->setToolTip(detail);
                perceptionLabel->setProperty(
                    "watching", phase == QStringLiteral("Watching"));
                perceptionLabel->style()->unpolish(perceptionLabel);
                perceptionLabel->style()->polish(perceptionLabel);
                AppendActivity(QStringLiteral("Perception ") + phase +
                    QStringLiteral(": ") + detail);
            }
            else
            {
                // An observation, not a state change. It goes to the activity feed so
                // there is a visible record of everything perception actually kept.
                AppendActivity(QStringLiteral("Saw (") + phase + QStringLiteral("): ") +
                    detail);
            }
        }
        else if (event.component == "Vision")
        {
            const QString phase = QString::fromStdString(event.phase);
            visionLabel->setText(QStringLiteral("Vision: ") + phase);
            visionLabel->setToolTip(QString::fromStdString(event.message));
            screenActionButton->setEnabled(
                phase != QStringLiteral("Disabled") && phase != QStringLiteral("Unavailable") &&
                phase != QStringLiteral("Error") && phase != QStringLiteral("Capturing") &&
                phase != QStringLiteral("Analyzing") && phase != QStringLiteral("Grounding") &&
                phase != QStringLiteral("Resolving") && !session.IsBusy());
            QString detail = QStringLiteral("Vision ") + phase + QStringLiteral(": ") +
                QString::fromStdString(event.message);
            if (event.elapsedMilliseconds >= 0.0)
            {
                detail += QStringLiteral(" (") +
                    QString::number(event.elapsedMilliseconds, 'f', 1) + QStringLiteral("ms)");
            }
            AppendActivity(detail);
        }
        else
        {
            QString detail = QString::fromStdString(event.component) + QStringLiteral(" ") +
                QString::fromStdString(event.phase) + QStringLiteral(": ") +
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
        }
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::AssistantMessage)
    {
        // A reply to merged voice input. Nobody is waiting on a return value for these,
        // so they arrive here instead. Held for their audio on the same terms as a typed
        // reply, since a non-zero turnId means speech is coming.
        const QString speaker = QString::fromStdString(session.DisplayName());
        const QString body = QString::fromStdString(event.message);
        const QString reasoning = QString::fromStdString(event.detail);
        if (event.turnId != 0 && event.turnId != lastSpeakingUtteranceId)
        {
            pendingUtterances[event.turnId] = {speaker, body, reasoning};
            pendingSpeechTimer->start();
        }
        else
        {
            AppendChat(speaker, body, false, reasoning);
        }
        return;
    }

    const QString message = QString::fromStdString(event.message);
    const QString timestamped = message.startsWith('[')
        ? message
        : '[' + EventTime(event) + "] " + message;
    AppendActivity(
        timestamped,
        event.kind == revia::runtime::RuntimeEventKind::Error
            ? ActivitySeverity::Error
            : (event.kind == revia::runtime::RuntimeEventKind::Warning
                ? ActivitySeverity::Warning
                : ActivitySeverity::Automatic));
}

void ReviaWindow::UpdateState(
    const revia::runtime::RuntimeState newState,
    const QString& detail)
{
    lastRuntimeState = newState;
    lastRuntimeDetail = detail;
    RefreshStateBadge();
}

void ReviaWindow::RefreshStateBadge()
{
    const revia::runtime::RuntimeState newState = lastRuntimeState;
    const QString& detail = lastRuntimeDetail;
    QString name = QString::fromStdString(revia::runtime::ToString(newState));
    QString color = StateColor(newState);
    // Speech outlives the turn that produced it. While it is queued, generating, or
    // playing, say so rather than reporting Idle at someone who is about to be spoken to.
    if (speechActive && newState == revia::runtime::RuntimeState::Idle)
    {
        name = speechPhase == QStringLiteral("Speaking")
            ? QStringLiteral("Speaking")
            : QStringLiteral("Preparing voice");
        color = QStringLiteral("#85d7c8");
    }
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
    const bool userMessage,
    const QString& reasoning)
{
    chatEntries.push_back({speaker, message, reasoning, userMessage, false});
    RenderChat();
}

void ReviaWindow::RenderChat()
{
    // Rebuilt in full rather than appended, because expanding one entry changes the
    // document and QTextBrowser has no way to toggle a region in place.
    QString html;
    html.reserve(4096);
    QString previousSpeaker;
    bool previousSpeakerWasUser = false;
    for (std::size_t index = 0; index < chatEntries.size(); ++index)
    {
        const ChatEntry& entry = chatEntries[index];
        const QString align = entry.userMessage ? "right" : "left";
        const QString speakerColour = entry.userMessage ? "#9dd7ff" : "#70e0ca";

        // Streamed speech arrives sentence by sentence. Keep those separate so each one
        // can appear when its audio begins, but do not print "Revia" above every sentence
        // when nobody else has spoken between them.
        const bool repeatedAssistantSpeaker = !entry.userMessage &&
            !previousSpeakerWasUser && !entry.speaker.isEmpty() &&
            entry.speaker == previousSpeaker;
        if (!entry.speaker.isEmpty() && !repeatedAssistantSpeaker)
        {
            html += QStringLiteral(
                "<p style=\"margin-top:8px; margin-bottom:2px; text-align:%1;\">"
                "<b><span style=\"color:%2;\">%3</span></b></p>")
                .arg(align, speakerColour, entry.speaker.toHtmlEscaped());
        }
        if (!entry.speaker.isEmpty())
        {
            previousSpeaker = entry.speaker;
            previousSpeakerWasUser = entry.userMessage;
        }

        if (!entry.body.isEmpty())
        {
            html += QStringLiteral(
                "<p style=\"margin-top:0px; margin-bottom:2px; text-align:%1; "
                "color:#dce9f7;\">%2</p>")
                .arg(align, HtmlParagraph(entry.body));
        }

        if (entry.reasoning.isEmpty())
        {
            continue;
        }
        // The anchor carries the entry index; anchorClicked toggles it and re-renders.
        html += QStringLiteral(
            "<p style=\"margin-top:0px; margin-bottom:2px; text-align:%1;\">"
            "<a href=\"thought:%2\" style=\"color:#8390a3; text-decoration:none;\">"
            "%3 Thought process</a></p>")
            .arg(align, QString::number(index),
                entry.expanded ? QStringLiteral("&#9662;") : QStringLiteral("&#9656;"));

        if (entry.expanded)
        {
            html += QStringLiteral(
                "<p style=\"margin-top:0px; margin-bottom:6px; color:#8390a3; "
                "font-size:11px;\">%1</p>").arg(HtmlParagraph(entry.reasoning));
        }
    }

    chatHistory->setHtml(html);
    chatHistory->verticalScrollBar()->setValue(chatHistory->verticalScrollBar()->maximum());
}

void ReviaWindow::AppendActivity(
    const QString& inputMessage,
    ActivitySeverity severity)
{
    QString message = inputMessage.trimmed();
    if (message.isEmpty())
    {
        return;
    }
    if (!message.startsWith(QLatin1Char('[')))
    {
        message = QStringLiteral("[") +
            QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")) +
            QStringLiteral("] ") + message;
    }
    if (severity == ActivitySeverity::Automatic)
    {
        const QString lowered = message.toLower();
        if (lowered.contains(QStringLiteral("] [error]")) ||
            lowered.contains(QStringLiteral(" error:")) ||
            lowered.contains(QStringLiteral(" failed")) ||
            lowered.contains(QStringLiteral(" failure")) ||
            lowered.contains(QStringLiteral(" could not")))
        {
            severity = ActivitySeverity::Error;
        }
        else if (lowered.contains(QStringLiteral("] [warning]")) ||
            lowered.contains(QStringLiteral(" warning")) ||
            lowered.contains(QStringLiteral(" degraded")) ||
            lowered.contains(QStringLiteral(" fallback")) ||
            lowered.contains(QStringLiteral(" unavailable")) ||
            lowered.contains(QStringLiteral(" blocked")) ||
            lowered.contains(QStringLiteral(" refused")) ||
            lowered.contains(QStringLiteral(" missing")))
        {
            severity = ActivitySeverity::Warning;
        }
        else
        {
            severity = ActivitySeverity::Information;
        }
    }

    activityEntries.push_back({message, severity});
    if (severity == ActivitySeverity::Warning)
    {
        ++activityWarningCount;
    }
    else if (severity == ActivitySeverity::Error)
    {
        ++activityErrorCount;
    }
    constexpr std::size_t MaximumVisibleLogEntries = 2000;
    bool trimmedOldEntries = false;
    if (activityEntries.size() > MaximumVisibleLogEntries)
    {
        trimmedOldEntries = true;
        activityEntries.erase(
            activityEntries.begin(),
            activityEntries.begin() +
                static_cast<std::ptrdiff_t>(activityEntries.size() - MaximumVisibleLogEntries));
        // Recounted from what is still held, because the counters describe the retained
        // model and trimming changes that model. Incrementing on append but never
        // decrementing on trim is what let the summary report errors that had already
        // scrolled out of existence -- at 2000 entries a full recount is trivial, and it
        // cannot drift the way paired increments and decrements can.
        std::vector<int> retained;
        retained.reserve(activityEntries.size());
        for (const ActivityEntry& entry : activityEntries)
        {
            retained.push_back(entry.severity == ActivitySeverity::Warning ? 1
                : entry.severity == ActivitySeverity::Error ? 2 : 0);
        }
        const revia::runtime::RetainedSeverityCounts counts =
            revia::runtime::CountRetainedSeverities(retained);
        activityWarningCount = counts.warnings;
        activityErrorCount = counts.errors;
    }
    const int filter = activityFilter == nullptr ? 0 : activityFilter->currentIndex();
    const bool visible = (filter == 0) ||
        (filter == 1 && severity != ActivitySeverity::Information) ||
        (filter == 2 && severity == ActivitySeverity::Error);
    if (trimmedOldEntries)
    {
        RenderActivity();
    }
    else if (visible && activityFeed != nullptr)
    {
        const QString color = severity == ActivitySeverity::Error
            ? QStringLiteral("#ff7b88")
            : (severity == ActivitySeverity::Warning
                ? QStringLiteral("#e8c56d")
                : QStringLiteral("#c7d5e8"));
        activityFeed->append(QStringLiteral(
            "<span style=\"color:%1; white-space:pre-wrap;\">%2</span>")
            .arg(color, message.toHtmlEscaped()));
        if (activityAutoScroll != nullptr && activityAutoScroll->isChecked())
        {
            activityFeed->verticalScrollBar()->setValue(
                activityFeed->verticalScrollBar()->maximum());
        }
    }
    UpdateActivitySummary();
}

void ReviaWindow::RenderActivity()
{
    if (activityFeed == nullptr || activityFilter == nullptr)
    {
        return;
    }
    const int previousScroll = activityFeed->verticalScrollBar()->value();
    const int filter = activityFilter->currentIndex();
    QString html;
    html.reserve(static_cast<qsizetype>(activityEntries.size() * 96));
    for (const ActivityEntry& entry : activityEntries)
    {
        if ((filter == 1 && entry.severity == ActivitySeverity::Information) ||
            (filter == 2 && entry.severity != ActivitySeverity::Error))
        {
            continue;
        }
        const QString color = entry.severity == ActivitySeverity::Error
            ? QStringLiteral("#ff7b88")
            : (entry.severity == ActivitySeverity::Warning
                ? QStringLiteral("#e8c56d")
                : QStringLiteral("#c7d5e8"));
        html += QStringLiteral(
            "<div style=\"color:%1; margin:0 0 4px 0; white-space:pre-wrap;\">%2</div>")
            .arg(color, entry.message.toHtmlEscaped());
    }
    activityFeed->setHtml(html);
    if (activityAutoScroll != nullptr && activityAutoScroll->isChecked())
    {
        activityFeed->verticalScrollBar()->setValue(
            activityFeed->verticalScrollBar()->maximum());
    }
    else
    {
        activityFeed->verticalScrollBar()->setValue(previousScroll);
    }
}

void ReviaWindow::UpdateActivitySummary()
{
    if (activityIssueSummary == nullptr)
    {
        return;
    }
    const bool hasErrors = activityErrorCount > 0;
    const bool hasWarnings = activityWarningCount > 0;
    activityIssueSummary->setText(!hasErrors && !hasWarnings
        ? QStringLiteral("No warnings or errors")
        : QStringLiteral("%1 error%2  •  %3 warning%4")
            .arg(activityErrorCount)
            .arg(activityErrorCount == 1 ? QString() : QStringLiteral("s"))
            .arg(activityWarningCount)
            .arg(activityWarningCount == 1 ? QString() : QStringLiteral("s")));
    // Labelled for what it counts. "Retained" is not a detail: the log holds the most
    // recent 2000 entries, so this is a count of what is still there rather than a
    // lifetime total, and conflating the two is what made the old number wrong.
    activityIssueSummary->setToolTip(QStringLiteral(
        "Warnings and errors in the retained activity log (most recent %1 entries).")
        .arg(2000));
    activityIssueSummary->setProperty("level", hasErrors
        ? QStringLiteral("error")
        : (hasWarnings ? QStringLiteral("warning") : QStringLiteral("ok")));
    activityIssueSummary->style()->unpolish(activityIssueSummary);
    activityIssueSummary->style()->polish(activityIssueSummary);

    const int runtimeIndex = tabs == nullptr ? -1 : tabs->indexOf(ui->runtimePage);
    if (runtimeIndex >= 0)
    {
        const int issueCount = activityErrorCount + activityWarningCount;
        tabs->setTabText(runtimeIndex, issueCount == 0
            ? QStringLiteral("Runtime")
            : QStringLiteral("Runtime (%1)").arg(issueCount));
        tabs->setTabToolTip(runtimeIndex, issueCount == 0
            ? QStringLiteral("No warnings or errors in the retained activity log.")
            : activityIssueSummary->text());
    }
}

void ReviaWindow::ApplyUserPreferences()
{
    const revia::runtime::UserPreferenceSnapshot snapshot = session.UserPreferences();
    const QSignalBlocker speechBlocker(speechCheck);
    const QSignalBlocker bargeInBlocker(bargeInCheck);
    const QSignalBlocker handsFreeBlocker(handsFreeCheck);
    const QSignalBlocker avatarBlocker(avatarBridgeCheck);
    const QSignalBlocker adapterBlocker(externalAdaptersCheck);
    const QSignalBlocker initiativeBlocker(initiativeCheck);
    const QSignalBlocker curiosityBlocker(curiosityCheck);
    const QSignalBlocker spontaneousBlocker(spontaneousSpeechCheck);
    const QSignalBlocker awayBlocker(speakWhenAwayCheck);
    const QSignalBlocker aiFilterBlocker(aiFilterCheck);
    const QSignalBlocker initiativeMaxBlocker(initiativeMaxSpin);
    const QSignalBlocker sampleBlocker(resourceSampleSpin);
    speechCheck->setChecked(snapshot.speechEnabled);
    bargeInCheck->setChecked(snapshot.bargeInEnabled);
    handsFreeCheck->setChecked(snapshot.handsFreeEnabled);
    avatarBridgeCheck->setChecked(snapshot.avatarBridgeEnabled);
    externalAdaptersCheck->setChecked(snapshot.externalAdaptersEnabled);
    autoSendVoiceCheck->setEnabled(!snapshot.handsFreeEnabled);
    initiativeCheck->setChecked(snapshot.initiativeEnabled);
    curiosityCheck->setChecked(snapshot.curiosityEnabled);
    spontaneousSpeechCheck->setChecked(snapshot.spontaneousSpeechEnabled);
    speakWhenAwayCheck->setChecked(snapshot.speakWhenUserAway);
    aiFilterCheck->setChecked(snapshot.aiResponseReviewEnabled);
    initiativeMaxSpin->setValue(snapshot.initiativeMaxPerHour);
    initiativeMaxSpin->setEnabled(snapshot.initiativeEnabled);
    curiosityCheck->setEnabled(snapshot.initiativeEnabled);
    spontaneousSpeechCheck->setEnabled(
        snapshot.initiativeEnabled && snapshot.curiosityEnabled);
    speakWhenAwayCheck->setEnabled(snapshot.initiativeEnabled && snapshot.curiosityEnabled);
    resourceSampleSpin->setValue(snapshot.resourceSampleSeconds);
    RefreshPresenceUi();
}

void ReviaWindow::RefreshPresenceUi()
{
    if (presencePhaseValue == nullptr) return;
    const revia::presence::PresenceSnapshot snapshot = session.Presence();
    presencePhaseValue->setText(QString::fromStdString(snapshot.phase));
    presenceAffectValue->setText(
        QString::fromStdString(revia::runtime::ToString(snapshot.affect)) +
        QStringLiteral(" %1%").arg(qRound(snapshot.affectIntensity * 100.0F)));
    presenceAttentionValue->setText(QString::fromStdString(snapshot.attention));
    presenceAttentionValue->setToolTip(QString::fromStdString(snapshot.attention));
    presenceMomentumBar->setValue(qRound(snapshot.conversationMomentum * 100.0F));
}

void ReviaWindow::ShowPreferenceResult(const revia::core::PreferenceResult& result)
{
    const QString message = QString::fromStdString(result.message);
    preferenceStatus->setText(message);
    preferenceStatus->setProperty("error", !result.succeeded);
    preferenceStatus->style()->unpolish(preferenceStatus);
    preferenceStatus->style()->polish(preferenceStatus);
    AppendActivity(
        QStringLiteral("Setting: ") + message,
        result.succeeded ? ActivitySeverity::Information : ActivitySeverity::Warning);
}

bool ReviaWindow::ConfirmAction(
    const revia::actions::ActionRequest& request,
    const revia::actions::PolicyDecision& decision)
{
    bool confirmed = false;
    const auto showConfirmation = [this, &request, &decision, &confirmed]()
    {
        // A screen action hides Revia before capture so it cannot obscure the target.
        // Restore the shell before asking; a confirmation owned by a minimized window is
        // effectively invisible and would leave the worker waiting forever.
        if (isMinimized())
        {
            showNormal();
            raise();
            activateWindow();
        }
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
        if (request.resolution.visionResolved)
        {
            description += "\n\nVision target: " +
                QString::fromStdString(request.resolution.modelTarget) +
                "\nResolved UIA element: " +
                QString::fromStdString(request.resolution.resolvedName) +
                "\nMatch confidence: " +
                QString::number(request.resolution.matchConfidence * 100.0, 'f', 1) + "%" +
                "\nThe exact UIA runtime id must still match when you approve.";
        }
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
