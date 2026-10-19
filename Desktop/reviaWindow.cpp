#include "reviaWindow.h"
#include "tabNavigation.h"

#include "Core/logger.h"
#include "Audit/contentDigest.h"
#include "answerFeedbackDialog.h"
#include "conversationStatusPanel.h"
#include "Runtime/retainedCounts.h"
#include "Speech/transcriptRouting.h"
#include "capabilityPanel.h"
#include "pipelinePanel.h"
#include "memoryPanel.h"
#include "mindPanel.h"
#include "visionPanel.h"
#include "voiceHealthPanel.h"
#include "profilePanel.h"
#include "agentStudioPanel.h"
#include "canvasPanel.h"
#include "internetActivityPanel.h"
#include "resourcePanel.h"
#include "chatMarkdown.h"
#include "chatSessionsPanel.h"
#include "commandPalette.h"
#include "composerCard.h"
#include "documentsDialog.h"
#include "panelWindow.h"
#include "serialWorker.h"
#include "settingsDialog.h"
#include "shellSidebar.h"
#include "theme.h"
#include "toastOverlay.h"
#include "ui_reviaWindow.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFormLayout>
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
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QMargins>
#include <QPushButton>
#include <QScrollArea>
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
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QtMath>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextFrameFormat>
#include <QThread>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QWindow>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <string>

namespace
{
constexpr const char* ExpiredAnswerReviewOutcome = "This displayed reply belongs to an earlier audience. Review a fresh reply.";

// Nothing thrown on a worker may leave it. An exception escaping a std::jthread body
// calls std::terminate and takes the window, and the conversation, with it -- which is
// why the typed turn's worker catches, and every other worker has to as well. The
// failure becomes the operation's own unsuccessful result, so the ordinary completion
// path re-enables the controls and says what went wrong.
template <typename Result, typename Operation, typename Failure> Result RunOrFail(Operation&& operation, Failure&& failure)
{
    try
    {
        return operation();
    }
    catch (const std::exception& error)
    {
        return failure(std::string(error.what()));
    }
    catch (...)
    {
        return failure(std::string("the failure did not say why"));
    }
}

// Clears a running flag when a worker body ends, however it ends, so the GUI thread
// can tell a finished worker from one it would have to wait for.
class RunningFlag
{
  public:
    explicit RunningFlag(std::atomic<bool>& flag) : flag(flag) {}
    ~RunningFlag()
    {
        flag.store(false);
    }
    RunningFlag(const RunningFlag&) = delete;
    RunningFlag& operator=(const RunningFlag&) = delete;

  private:
    std::atomic<bool>& flag;
};

namespace
{
// What reading one attached file came to, carried from the worker to the GUI thread.
struct AttachmentOutcome
{
    bool accepted = false;
    QString message;
    AttachmentChip chip;
};

AttachmentOutcome OutcomeOf(const revia::attachments::IntakeResult& result, const QImage& thumbnail)
{
    AttachmentOutcome outcome;
    outcome.accepted = result.status == revia::attachments::IntakeStatus::Accepted;
    outcome.message = QString::fromStdString(result.message);
    if (outcome.accepted)
    {
        outcome.chip.id = result.attachment.id;
        outcome.chip.name = QString::fromStdString(result.attachment.displayName);
        outcome.chip.size = QString::fromStdString(revia::attachments::HumanSize(result.attachment.bytes));
        outcome.chip.thumbnail = thumbnail;
    }
    return outcome;
}

QStringList LocalFiles(const QMimeData* data)
{
    QStringList files;
    if (data == nullptr || !data->hasUrls())
        return files;
    for (const QUrl& url : data->urls())
        if (url.isLocalFile())
            files << url.toLocalFile();
    return files;
}
} // namespace

revia::speech::VoiceOperationResult FailedVoiceOperation(const std::string&)
{
    return {false, "The voice operation failed. The cause is unknown.", {}, -1.0};
}

// What the state pill shows for a runtime state (design spec 2.4). Idle is refined by the
// microphone and speech in RefreshStateBadge; the stylesheet colours the dot by `state`.
struct StatePresentation
{
    const char* state;
    QString label;
};

StatePresentation PresentState(const revia::runtime::RuntimeState state, const bool desktopControl)
{
    using revia::runtime::RuntimeState;
    switch (state)
    {
    case RuntimeState::Offline:
        return {"offline", QStringLiteral("Offline")};
    case RuntimeState::Starting:
        return {"starting", QStringLiteral("Starting…")};
    case RuntimeState::Stopping:
        return {"stopping", QStringLiteral("Stopping…")};
    case RuntimeState::Error:
        return {"error", QStringLiteral("Problem")};
    case RuntimeState::WaitingForConfirmation:
        return {"waiting", QStringLiteral("Needs your OK")};
    case RuntimeState::Acting:
        return desktopControl ? StatePresentation{"acting", QStringLiteral("Controlling desktop")}
                              : StatePresentation{"busy", QStringLiteral("Working")};
    case RuntimeState::Blocked:
        return {"blocked", QStringLiteral("Blocked")};
    case RuntimeState::Thinking:
        return {"busy", QStringLiteral("Thinking")};
    case RuntimeState::Responding:
        return {"busy", QStringLiteral("Replying")};
    case RuntimeState::Remembering:
        return {"busy", QStringLiteral("Remembering")};
    case RuntimeState::Idle:
    default:
        return {"ready", QStringLiteral("Ready")};
    }
}

// Dynamic properties only take effect in the stylesheet after a repolish.
void Repolish(QWidget* widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
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
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(event.occurredAt.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(milliseconds).toString("HH:mm:ss.zzz");
}
}

ReviaWindow::ReviaWindow(const bool startRuntime, const bool buildSystemTray, QWidget* parent)
    : QMainWindow(parent), ui(std::make_unique<Ui::ReviaWindow>())
{
    setWindowTitle("Revia");
    setWindowIcon(CreateReviaIcon());
    // The native frame brings edge resizing, snap layouts, the system shadow and Alt+Space.
    resize(1180, 780);
    setMinimumSize(760, 540);

    if (startRuntime)
    {
        companions = std::make_unique<revia::runtime::CompanionRegistry>(Session().Paths().InstallRoot());
        std::string error;
        if (companions->Load(error))
        {
            const auto selected = companions->Find(companions->SelectedId());
            if (selected && !selected->legacy)
            {
                session.Authority()->SetCompanionDefaults(selected->id, revia::policy::AuthorityPermissions::WithinMachineCeiling());
                initializedCompanionAuthority.insert(selected->id);
                selectedSession = std::make_unique<revia::runtime::ReviaSession>(
                    revia::runtime::CompanionPaths(Session().Paths().InstallRoot(), *selected), session.Authority());
            }
        }
    }

    // Pages first, then the windows that host them, then the shell around the chat.
    BuildInterface();
    BuildCompanionControls();
    BuildPanels();
    BuildSettingsDialog();
    BuildShell();
    ApplyResponsiveLayout();
    if (buildSystemTray)
    {
        BuildTray();
    }

    BindSession();

    // Upper bound on how long a reply waits for its audio. Qwen generation can take
    // seconds; silence for longer than this reads as the reply having been lost.
    pendingSpeechTimer = new QTimer(this);
    pendingSpeechTimer->setSingleShot(true);
    pendingSpeechTimer->setInterval(9000);
    connect(pendingSpeechTimer, &QTimer::timeout, this,
        [this]()
        {
            // Zero: release everything still waiting. A stalled voice must not cost the user
            // the reply itself.
            ReleasePendingSpeechText(0);
        });

    pollTimer = new QTimer(this);
    pollTimer->setInterval(250);
    connect(pollTimer, &QTimer::timeout, this,
        [this]()
        {
            if (switchingCompanion)
                return;
            Session().PollBackgroundEvents();
            if (agentStudioPanel)
                agentStudioPanel->SetSnapshot(Session().AgentWorkflowSnapshot());
        });
    pollTimer->start();

    ApplyMicrophoneUi(MicrophoneUi::Unavailable);
    // The message box has the keyboard when the window first shows.
    messageInput->setFocus(Qt::OtherFocusReason);
    if (startRuntime)
    {
        StartRuntime();
        // The owner's choice in Appearance: panels that were open when Revia last closed come back
        // beside the chat, without taking the keyboard from it.
        if (QSettings().value("panels/restoreOpen", false).toBool())
            QTimer::singleShot(0, this, [this]() { panelHost->RestoreOpenSet(); });
    }
    else
    {
        UpdateState(revia::runtime::RuntimeState::Offline, "UI smoke-test mode");
        stopButton->setEnabled(false);
        UpdateSendState();
    }
}

ReviaWindow::~ReviaWindow()
{
    // First: the joins below run on this thread, which will never show another dialog,
    // so a worker waiting for one must be refused rather than wait for ever.
    shuttingDown.store(true);
    sessionUiEpoch.fetch_add(1);
    if (themeListenerId >= 0)
        revia::desktop::theme::RemoveListener(themeListenerId);
    AbandonQuestions();
    if (shutdownWorker.joinable())
        shutdownWorker.join();
    if (companionSwitchWorker.joinable())
        companionSwitchWorker.join();
    // Settings writes run against the session; they finish before it stops.
    if (settingsWorker)
        settingsWorker->Stop();
    Session().Events().Unsubscribe(subscriptionId);
    Session().RequestStop();
    if (studioWorker.joinable())
    {
        studioWorker.request_stop();
        studioWorker.join();
    }
    if (operationWorker.joinable())
    {
        operationWorker.join();
    }
    if (capabilityWorker.joinable())
    {
        capabilityWorker.join();
    }
    if (deviceWorker.joinable())
    {
        deviceWorker.join();
    }
    Session().Stop();
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
    return Session().IsStarted();
}

bool ReviaWindow::eventFilter(QObject* watched, QEvent* event)
{
    if ((messageInput && watched == messageInput->viewport()) || (chatHistory && watched == chatHistory->viewport()))
    {
        if (HandleAttachmentDrag(event))
            return true;
        // The measure, and an empty chat's greeting height, follow the viewport without
        // re-rendering the transcript.
        if (chatHistory && watched == chatHistory->viewport() && event->type() == QEvent::Resize)
            ApplyChatMeasure();
    }
    if (watched == chatHistory && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut))
    {
        // The transcript's ring belongs to the keyboard: a click into it to select text shows none.
        const bool before = chatHistory->property("kbdFocus").toBool();
        const bool ring = revia::desktop::theme::KeyboardFocusAfter(before, *static_cast<QFocusEvent*>(event));
        if (ring != before)
        {
            chatHistory->setProperty("kbdFocus", ring);
            Repolish(chatHistory);
        }
    }
    if (watched == chatColumn && event->type() == QEvent::Resize)
    {
        // The overlays float over the column rather than sit in its layout.
        if (navigationLockOverlay)
            navigationLockOverlay->setGeometry(chatColumn->rect());
        ApplyComposerMeasure();
        ApplyResponsiveLayout();
    }
    if ((watched == chatHeader && event->type() == QEvent::Resize) ||
        (composerShadow && watched == composerShadow && (event->type() == QEvent::Move || event->type() == QEvent::Resize)))
    {
        RefreshChatTitle();
        if (jumpToLatest && composerShadow)
        {
            jumpToLatest->adjustSize();
            const QPoint composerTop = composerShadow->mapTo(chatColumn, QPoint(0, 0));
            jumpToLatest->move((chatColumn->width() - jumpToLatest->width()) / 2, composerTop.y() - jumpToLatest->height() - 8);
            jumpToLatest->raise();
        }
    }
    if (settingsDialog && watched == settingsDialog && event->type() == QEvent::Show)
    {
        // The dialog's own window gets the native caption colours on its first show.
        if (!settingsDialog->property("reviaFramed").toBool())
        {
            settingsDialog->setProperty("reviaFramed", true);
            revia::desktop::theme::ApplyNativeFrame(settingsDialog);
        }
        RefreshSettingsExtras();
    }
    if (watched == messageInput && event->type() == QEvent::KeyPress)
    {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->matches(QKeySequence::Paste))
        {
            // Copied files attach; copied text pastes as text even when the copying app
            // also put a picture of it on the clipboard; a bare picture attaches.
            const QMimeData* data = QApplication::clipboard()->mimeData();
            if (const QStringList files = LocalFiles(data); !files.isEmpty())
            {
                AttachPaths(files);
                return true;
            }
            if (data && data->hasImage() && data->text().trimmed().isEmpty())
            {
                AttachPastedImage(qvariant_cast<QImage>(data->imageData()));
                return true;
            }
        }
        if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) && !keyEvent->modifiers().testFlag(Qt::ShiftModifier))
        {
            SendMessage();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Escape)
        {
            // An edit in progress goes first; otherwise Esc stops a running turn, task or studio
            // operation (Stop in Send's slot). A reflex key in a text box never cancels the startup,
            // speech or the microphone: those stop from their own button.
            if (editingLastTurn)
            {
                CancelEditLastTurn();
                return true;
            }
            if (stopButton->isEnabled() && stopButton->property("primary").toBool())
            {
                stopButton->click();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void ReviaWindow::closeEvent(QCloseEvent* event)
{
    // Shutdown ends with QApplication::quit(), which first closes every visible window and gives
    // up if one refuses. Once shutdown has begun the close is therefore accepted, or the process
    // would stay alive behind a locked window.
    if (shuttingDown.load())
    {
        event->accept();
        return;
    }
    if (trayIcon && trayIcon->isVisible() && QSettings().value("window/closeToTray", true).toBool())
    {
        hide();
        if (panelHost)
            panelHost->HideAll(true);
        QSettings settings;
        if (!settings.value("window/trayHintShown", false).toBool())
        {
            settings.setValue("window/trayHintShown", true);
            trayIcon->showMessage("Revia", "Revia is still running here. Use Quit Revia from the tray when you want it to stop.",
                QSystemTrayIcon::Information, 2500);
        }
        event->ignore();
        return;
    }
    // Nowhere to retreat to, or the owner asked closing to quit: shut down properly. Accepting the
    // close here would leave a windowless process with every server still running; the window
    // stays up under "Closing Revia…" until the shutdown's quit() closes it. BeginShutdown saves
    // the open panels before it hides them.
    BeginShutdown(revia::core::ExitReason::WindowClosed, "the window was closed");
    event->ignore();
}

void ReviaWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    // The caption takes the scheme's colours once the native window exists.
    if (!property("reviaFramed").toBool())
    {
        setProperty("reviaFramed", true);
        revia::desktop::theme::ApplyNativeFrame(this);
    }
}

void ReviaWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    ApplyResponsiveLayout();
    // The message box grows with its text up to eight lines, 200px or 30% of the window.
    if (composer)
        composer->SetMaxInputHeight(std::min(200, std::max(48, height() * 3 / 10)));
}

namespace
{
// Moves a page's existing content into a scroll area without rebuilding it.
//
// Presence and Memory are stacks of group boxes with nothing stretchy in them: when the
// window is shorter than the content, a plain layout has nowhere to take the shortfall from
// and compresses every child below its minimum, so labels land on top of one another.
// Content this tall has to be scrollable rather than squeezable.
void MakePageScrollable(QWidget* page, const char* scrollName = nullptr)
{
    if (page == nullptr)
        return;
    QLayout* existing = page->layout();
    if (existing == nullptr)
        return;

    auto* inner = new QWidget();
    inner->setObjectName("scrolledPage");
    // Reparents the layout and every widget already inside it.
    inner->setLayout(existing);

    auto* scroll = new QScrollArea(page);
    if (scrollName)
        scroll->setObjectName(scrollName);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(inner);

    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
}
} // namespace

void ReviaWindow::BuildInterface()
{
    // Palette, stylesheet and app font for the light or dark scheme, following Windows unless
    // the owner chose one. First, so every widget the .ui makes is polished once, in the right
    // scheme; the window and everything parented to it restyle on a change.
    revia::desktop::theme::Install(this);
    ui->setupUi(this);

    chatPage = ui->chatPage;
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
    renderVoiceBankButton = ui->renderVoiceBankButton;

    // The microphone controls from the old Settings page, in one row the Settings dialog hosts
    // under Voice. Same names and wiring as before.
    microphoneRow = new QWidget(this);
    microphoneRow->setObjectName("microphoneRow");
    auto* microphoneLayout = new QBoxLayout(QBoxLayout::LeftToRight, microphoneRow);
    microphoneLayout->setContentsMargins(0, 0, 0, 0);
    microphoneLayout->setSpacing(8);
    microphoneDeviceCombo = new QComboBox(microphoneRow);
    microphoneDeviceCombo->setObjectName("microphoneDeviceCombo");
    microphoneDeviceCombo->setAccessibleName("Microphone");
    microphoneDeviceCombo->setToolTip("Which Windows recording device is used. Default follows the Windows setting. A saved device that "
                                      "is unplugged falls back to Default and says so rather than switching silently.");
    microphoneDeviceCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    microphoneDeviceCombo->setMinimumContentsLength(14);
    microphoneLayout->addWidget(microphoneDeviceCombo, 1);
    refreshMicrophonesButton = new QPushButton("Refresh", microphoneRow);
    refreshMicrophonesButton->setObjectName("refreshMicrophonesButton");
    refreshMicrophonesButton->setToolTip("Re-read the list of recording devices Windows currently reports.");
    microphoneLayout->addWidget(refreshMicrophonesButton);
    testMicrophoneButton = new QPushButton("Test microphone", microphoneRow);
    testMicrophoneButton->setObjectName("testMicrophoneButton");
    testMicrophoneButton->setToolTip("Record a few seconds from the selected device, measure the signal, and transcribe it. The result "
                                     "is shown here only and is never sent as a message.");
    microphoneLayout->addWidget(testMicrophoneButton);
    microphoneTestResultLabel = new QLabel(this);
    microphoneTestResultLabel->setObjectName("microphoneTestResultLabel");
    microphoneTestResultLabel->setWordWrap(true);
    microphoneTestResultLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    microphoneTestResultLabel->setParent(microphoneRow);
    microphoneLayout->addWidget(microphoneTestResultLabel, 1);

    chatVoiceHealth = new VoiceHealthPanel(true, chatPage);
    conversationStatus = new ConversationStatusPanel(chatPage);
    connect(conversationStatus->ReviewButton(), &QPushButton::clicked, this, [this]() { ReviewDisplayedAnswer(); });
    if (auto* details = conversationStatus->findChild<QPushButton*>("conversationDetailsButton"))
        connect(details, &QPushButton::toggled, this, [this]() { UpdateConversationStrip(); });
    BuildChatSidebar();
    voiceHealth = new VoiceHealthPanel(false, ui->voicePage);
    ui->voiceLayout->insertWidget(0, voiceHealth);
    connect(chatVoiceHealth->DetailsButton(), &QPushButton::clicked, this,
        [this]()
        {
            OpenPanel(revia::desktop::PanelId::Voice);
            ui->voiceScroll->verticalScrollBar()->setValue(0);
        });

    messageInput->installEventFilter(this);
    chatHistory->installEventFilter(this);
    BuildAttachmentControls();

    pipelinePanel = new PipelinePanel(ui->pipelinePage);
    ui->pipelineHostLayout->addWidget(pipelinePanel);
    internetActivityPanel = new InternetActivityPanel(ui->internetActivityPage);
    ui->internetActivityHostLayout->addWidget(internetActivityPanel);
    resourcePanel = new ResourcePanel(
        [this](const std::string& device) { return Session().SetPreference("resources.voiceDevice", device); }, ui->resourcePage);
    ui->resourceHostLayout->addWidget(resourcePanel);
    canvasPanel = new CanvasPanel(ui->canvasPage);
    ui->canvasHostLayout->addWidget(canvasPanel);
    // Hosted by Settings › Permissions (and its internet cards by Settings › Internet) once the
    // dialog exists; the dialog's own page scrolls, so the panel needs no scroll area of its own.
    capabilityPanel = new CapabilityPanel(Session(), [this]() { DiscoverApplicationPermissions(); }, this);
    profilePanel = new ProfilePanel(Session(), ui->profilesPage);
    ui->profilesHostLayout->addWidget(profilePanel);
    memoryPanel = new MemoryPanel(Session(), ui->memoryTab);
    ui->memoryHostLayout->addWidget(memoryPanel);
    ConfigureMemoryRevisionControls();
    mindPanel = new MindPanel(Session(), ui->mindTab);
    ui->mindHostLayout->addWidget(mindPanel);
    visionPanel = new VisionPanel(Session(), ui->visionTab);
    ui->visionHostLayout->addWidget(visionPanel);

    voiceLanguageCombo->addItems(
        {"English", "Chinese", "Japanese", "Korean", "German", "French", "Russian", "Portuguese", "Spanish", "Italian"});

    // Presence's switches are Settings rows now; the page says where they went.
    auto* presenceSettings = new QPushButton("Open Settings", ui->presenceControlsGroup);
    presenceSettings->setObjectName("presenceOpenSettings");
    presenceSettings->setToolTip("Open Settings › Integrations");
    ui->presenceControlsLayout->addWidget(presenceSettings, 0, Qt::AlignLeft);
    connect(presenceSettings, &QPushButton::clicked, this, [this]() { OpenSettings(QStringLiteral("Integrations")); });

    connect(chatHistory, &QTextBrowser::anchorClicked, this,
        [this](const QUrl& link)
        {
            // Every anchor here was written by RenderChat; reply text cannot add one. A
            // saved chat on screen has none, and its indices would not be the live ones.
            if (!viewingArchivedChat.empty())
                return;
            const QString target = link.toString();
            const qsizetype colon = target.indexOf(QLatin1Char(':'));
            if (colon <= 0)
                return;
            const QString action = target.left(colon);
            const QString argument = target.mid(colon + 1);
            // A link that would start or change a turn waits for the one in flight; reading
            // the details of a panel, or what Revia is checking right now, never does.
            if (action == QStringLiteral("panel"))
            {
                if (const auto panel = revia::desktop::PanelFromKey(argument))
                    OpenPanel(*panel);
                return;
            }
            bool parsed = false;
            const std::size_t index = static_cast<std::size_t>(argument.toULongLong(&parsed));
            if (action == QStringLiteral("thought"))
            {
                if (!parsed || index >= chatEntries.size())
                    return;
                chatEntries[index].expanded = !chatEntries[index].expanded;
                // Opening a block never moves the reader.
                renderKeepsPosition = true;
                RenderChat();
                renderKeepsPosition = false;
                return;
            }
            if (chatTurnInFlight)
                return;
            if (action == QStringLiteral("retry"))
            {
                RetryFailedTurn();
                return;
            }
            if (!parsed || index >= chatEntries.size())
                return;
            if (action == QStringLiteral("copy"))
            {
                QApplication::clipboard()->setText(MessageText(index));
                ShowStatus(QStringLiteral("Copied to the clipboard."), 3000);
            }
            else if (action == QStringLiteral("regen") && RegenerableReply() == index)
            {
                RetryLastExchange(QString());
            }
            else if (action == QStringLiteral("edit") && LastUserEntry() == index)
            {
                BeginEditLastTurn();
            }
            else if (action == QStringLiteral("review"))
            {
                ReviewDisplayedAnswer();
            }
        });
    // Copy for any reply, not only the newest one that carries the link.
    chatHistory->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(chatHistory, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) { CopyMessageAt(position); });
    connect(chatHistory->verticalScrollBar(), &QScrollBar::valueChanged, this,
        [this](const int value)
        {
            // Reaching the bottom again follows the conversation once more.
            const QScrollBar* bar = chatHistory->verticalScrollBar();
            if (bar->maximum() - value <= 48)
            {
                transcriptPinned = true;
                if (jumpToLatest)
                    jumpToLatest->hide();
            }
            else if (!renderingTranscript)
            {
                transcriptPinned = false;
            }
        });

    QSettings shellSettings;
    const int savedActivityFilter = std::clamp(shellSettings.value("activity/filter", 0).toInt(), 0, activityFilter->count() - 1);
    activityFilter->setCurrentIndex(savedActivityFilter);
    activityAutoScroll->setChecked(shellSettings.value("activity/follow", true).toBool());
    connect(activityFilter, &QComboBox::currentIndexChanged, this,
        [this](const int index)
        {
            QSettings().setValue("activity/filter", index);
            RenderActivity();
        });
    connect(activityAutoScroll, &QCheckBox::toggled, this,
        [this](const bool enabled)
        {
            // The same setting as Settings › General › Follow new activity.
            QSettings().setValue("activity/follow", enabled);
            if (settingsDialog)
                settingsDialog->SetValue("activity/follow", enabled);
        });
    connect(clearActivityButton, &QPushButton::clicked, this,
        [this]()
        {
            activityEntries.clear();
            lastComponentIssues.clear();
            activityWarningCount = 0;
            activityErrorCount = 0;
            RenderActivity();
            UpdateActivitySummary();
        });
    connect(openLogsButton, &QPushButton::clicked, this,
        [this]()
        {
            const QString path = QDir::current().filePath(QString::fromStdString(ReviaLogDirectory()));
            QDir().mkpath(path);
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
            {
                AppendActivity(QStringLiteral("The Logs folder could not be opened: ") + path, ActivitySeverity::Warning);
            }
        });

    connect(sendButton, &QPushButton::clicked, this, [this]() { SendMessage(); });
    connect(stopButton, &QPushButton::clicked, this, [this]()
    {
        // Remembered so the turn that ends now reads "Stopped" rather than as a failure.
        userStopRequested = true;
        studioWorker.request_stop();
        Session().RequestStop();
    });
    connect(microphoneButton, &QPushButton::clicked, this, [this]() { ToggleListening(); });
    connect(screenActionButton, &QPushButton::clicked, this, [this]() { UseVisibleScreen(); });
    connect(messageInput, &QPlainTextEdit::textChanged, this, [this]() { UpdateSendState(); });
    RefreshMicrophoneDevices();
    connect(refreshMicrophonesButton, &QPushButton::clicked, this, [this]() { RefreshMicrophoneDevices(); });
    connect(testMicrophoneButton, &QPushButton::clicked, this, [this]() { RunMicrophoneTest(); });
    connect(microphoneDeviceCombo, &QComboBox::currentIndexChanged, this,
        [this](const int index)
        {
            if (index < 0 || microphoneDeviceCombo->signalsBlocked())
            {
                return;
            }
            const QString device = microphoneDeviceCombo->itemData(index).toString();
            QSettings().setValue("input/microphoneDevice", device);
            Session().SetMicrophoneDevice(device.toStdString());
        });
    connect(openPresenceFolderButton, &QPushButton::clicked, this,
        [this]()
        {
            const QString path = QDir::current().filePath(QStringLiteral("RuntimeData/Presence"));
            QDir().mkpath(path);
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
            {
                AppendActivity(QStringLiteral("The Presence folder could not be opened: ") + path, ActivitySeverity::Warning);
            }
        });
    connect(createVoiceButton, &QPushButton::clicked, this, [this]() { CreateVoicePreset(); });
    connect(previewVoiceButton, &QPushButton::clicked, this, [this]() { PreviewVoice(); });
    connect(renderVoiceBankButton, &QPushButton::clicked, this, [this]() { RenderVoiceBank(); });
    RefreshVoiceStudio();

    MakePageScrollable(ui->presencePage);
    chatHistory->setMinimumHeight(120);
    chatHistory->setFrameShape(QFrame::NoFrame);
    MakePageScrollable(chatPage, "conversationPageScroll");
    MakePageScrollable(ui->memoryTab, "memoryPageScroll");
    conversationScroll = chatPage->findChild<QScrollArea*>("conversationPageScroll");
    // Long form rows wrap instead of widening their page (design spec 2.11).
    for (auto* form : findChildren<QFormLayout*>())
    {
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    }
    for (auto* label : {ui->avatarPathValue, ui->adapterInboxValue, ui->adapterOutboxValue})
    {
        label->setWordWrap(true);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
}

void ReviaWindow::BuildTray()
{
    trayIcon = new QSystemTrayIcon(CreateReviaIcon(), this);
    auto* trayMenu = new QMenu(this);
    QAction* openAction = trayMenu->addAction("Open Revia");
    QAction* alwaysOnTopAction = trayMenu->addAction("Always on top");
    alwaysOnTopAction->setCheckable(true);
    trayMenu->addSeparator();
    QAction* quitAction = trayMenu->addAction("Quit Revia");
    trayIcon->setContextMenu(trayMenu);
    trayIcon->setToolTip("Revia - Offline");

    // Panels hidden with the window come back with it, without taking the keyboard from the chat.
    const auto reopen = [this]()
    {
        showNormal();
        raise();
        activateWindow();
        if (panelHost)
            panelHost->RestoreHidden();
    };
    connect(openAction, &QAction::triggered, this, reopen);
    connect(alwaysOnTopAction, &QAction::toggled, alwaysOnTopCheck, &QCheckBox::setChecked);
    connect(alwaysOnTopCheck, &QCheckBox::toggled, alwaysOnTopAction, &QAction::setChecked);
    alwaysOnTopAction->setChecked(alwaysOnTopCheck->isChecked());
    connect(quitAction, &QAction::triggered, this, [this]() { BeginShutdown(revia::core::ExitReason::TrayQuit); });
    connect(trayIcon, &QSystemTrayIcon::activated, this,
        [reopen](const QSystemTrayIcon::ActivationReason reason)
        {
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                reopen();
        });
    trayIcon->show();
}

bool ReviaWindow::ClaimOperationWorker()
{
    if (operationRunning.exchange(true))
    {
        ShowStillWorking();
        return false;
    }
    // Finished: its own last act was clearing the flag, so this returns at once.
    if (operationWorker.joinable())
        operationWorker.join();
    return true;
}

void ReviaWindow::StartRuntime()
{
    if (switchingCompanion || shuttingDown.load() || !ClaimOperationWorker())
        return;
    stopButton->setEnabled(true);
    UpdateSendState();
    operationWorker = std::jthread(
        [this, epoch = sessionUiEpoch.load()]()
        {
            const RunningFlag running(operationRunning);
            std::string failure;
            const bool started = RunOrFail<bool>([this]() { return Session().Start(); },
                [&failure](const std::string& why)
                {
                    failure = why;
                    return false;
                });
            const QString greeting = QString::fromStdString(Session().Greeting());
            QMetaObject::invokeMethod(
                this,
                [this, epoch, started, greeting, failure]()
                {
                    if (epoch != sessionUiEpoch.load() || shuttingDown.load())
                        return;
                    screenActionButton->setEnabled(Session().IsVisionAvailable());
                    // Re-evaluate the microphone now that IsStarted() is finally true; the last
                    // recognition phase event arrived while startup was still in flight.
                    ApplyMicrophoneUi(microphoneUiState);
                    if (!greeting.isEmpty())
                    {
                        AppendChat(QString::fromStdString(Session().DisplayName()), greeting);
                    }
                    if (!started)
                    {
                        AppendActivity(failure.empty() ? QStringLiteral("Startup did not complete. Check the activity log.")
                                                       : QStringLiteral("Startup did not complete: ") + QString::fromStdString(failure),
                            ActivitySeverity::Error);
                    }
                    else
                    {
                        ApplyUserPreferences();
                    }
                    RefreshVoiceStudio();
                    if (resourcePanel != nullptr && started)
                    {
                        resourcePanel->SetVoiceDevicePreference(Session().VoiceDevicePreference());
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
                    RefreshChatSidebar();
                    RefreshAccount();
                    RefreshStateBadge();
                    RenderChat();
                    messageInput->setFocus();
                },
                Qt::QueuedConnection);
        });
}

void ReviaWindow::SendMessage(const bool voiceInput)
{
    // Voice may arrive while Revia is still replying; that is what the merge window is
    // for. Typed input still waits, because pressing Enter mid-reply and having it
    // silently queue would be surprising. The text stays in the box and the status line
    // says why, since Enter reaches here even while the Send button is disabled.
    if (switchingCompanion || shuttingDown.load())
    {
        return;
    }
    if (!voiceInput && (operationRunning.load() || Session().IsBusy()))
    {
        ShowStillWorking();
        return;
    }

    const QString text = messageInput->toPlainText().trimmed();
    // Typed messages only: a voice turn never picks up files waiting in the tray.
    const bool withAttachments = !voiceInput && attachmentTray && !attachmentTray->IsEmpty();
    if (text.isEmpty() && (!withAttachments || editingLastTurn))
    {
        return;
    }
    if (withAttachments && attachmentRunning.load())
    {
        ShowStatus(QStringLiteral("Still reading an attachment. Send again in a moment."), 6000);
        return;
    }
    messageInput->clear();
    // A new turn: whatever went wrong with the last one, and a stop asked for it, are history.
    lastTurnError.reset();
    userStopRequested = false;
    // A message always goes to the current chat, so that is what the window shows.
    CloseArchivedChat();
    if (!voiceInput && editingLastTurn)
    {
        editingLastTurn = false;
        if (composer)
            composer->ClearContext();
        composerContextKind = ComposerContextKind::None;
        RetryLastExchange(text);
        return;
    }

    if (voiceInput)
    {
        // Offered, not submitted. It merges with anything else said in the next moment,
        // and the reply arrives as an event once that window closes.
        const revia::agents::InputVerdict verdict = Session().OfferInput(text.toStdString(), revia::agents::InputSource::Voice);
        if (verdict == revia::agents::InputVerdict::Queued)
        {
            AppendChat("You", text, true);
        }
        else
        {
            // Shown rather than dropped in silence, so an over-eager filter is visible.
            AppendActivity(QStringLiteral("Heard but not answered: \"") + text + QStringLiteral("\" - ") +
                           QString::fromStdString(revia::agents::ToString(verdict)));
        }
        messageInput->setFocus();
        return;
    }

    if (!ClaimOperationWorker())
    {
        messageInput->setPlainText(text);
        return;
    }
    // Taken now: once the message is on its way the files belong to that turn.
    std::vector<AttachmentChip> attached;
    std::vector<std::string> attachmentIds;
    if (withAttachments && !editingLastTurn)
    {
        attached = attachmentTray->Chips();
        attachmentIds = attachmentTray->Ids();
        attachmentTray->Clear();
        attachmentTray->SetStatus(QString());
    }
    if (studioOperationRunning.load())
    {
        // The turn waits for the studio operation's lock on its own worker; the window
        // stays responsive and says what the message is waiting for.
        statusBar()->showMessage(QStringLiteral("Your message will be answered when the current studio operation finishes."), 8000);
        ShowComposerContext(QStringLiteral("Queued until the current studio operation finishes"), QString(), {});
        composerContextKind = ComposerContextKind::Queued;
    }
    chatTurnInFlight = true;
    if (attached.empty())
    {
        AppendChat("You", text, true);
    }
    else
    {
        ChatEntry entry;
        entry.speaker = QStringLiteral("You");
        entry.body = text;
        entry.userMessage = true;
        entry.attachments = attached;
        chatEntries.push_back(std::move(entry));
        TrimTranscript();
        RenderChat();
    }
    stopButton->setEnabled(true);
    UpdateSendState();

    operationWorker = std::jthread(
        [this, epoch = sessionUiEpoch.load(), input = text.toStdString(), voiceInput, origin = Session().Stamp(),
            audienceRevision = Session().Audience().revision, attachmentIds]()
        {
            const RunningFlag running(operationRunning);
            // Nothing thrown here may leave this thread.
            //
            // An exception escaping a std::jthread body calls std::terminate, so a single
            // malformed turn takes the whole desktop app with it -- no message, no chance to
            // retry, and the conversation gone. That was reachable: context compaction could
            // cut a multi-byte character in half, and serializing the resulting string threw
            // from inside Submit. The cut is fixed at its source, but a turn failing is a
            // normal outcome and must read as one wherever it comes from.
            revia::runtime::SessionResult result;
            result.stamp = origin;
            result.audienceRevision = audienceRevision;
            try
            {
                result = attachmentIds.empty()
                             ? Session().Submit(input, voiceInput ? revia::agents::InputSource::Voice : revia::agents::InputSource::Typed)
                             : Session().SubmitWithAttachments(input, attachmentIds);
            }
            catch (const std::exception& error)
            {
                result.succeeded = false;
                result.fromAssistant = false;
                result.text = "That turn could not be completed: " + std::string(error.what());
                result.reason = result.text;
            }
            catch (...)
            {
                result.succeeded = false;
                result.fromAssistant = false;
                result.text = "That turn could not be completed, and the failure did not "
                              "say why.";
                result.reason = result.text;
            }
            QMetaObject::invokeMethod(
                this,
                [this, epoch, result]()
                {
                    if (epoch != sessionUiEpoch.load() || shuttingDown.load())
                        return;
                    chatTurnInFlight = false;
                    if (composer)
                        composer->ClearContext();
                    PresentSessionResult(result);
                    RenderChat();
                    RefreshStateBadge();
                    RefreshChatSidebar();
                    RefreshChatTitle();
                    if (result.shouldExit)
                    {
                        BeginShutdown(revia::core::ExitReason::UserCommand, "the conversation asked Revia to exit");
                    }
                    else
                    {
                        messageInput->setFocus();
                    }
                },
                Qt::QueuedConnection);
        });
}

void ReviaWindow::PresentSessionResult(const revia::runtime::SessionResult& result)
{
    if ((!result.stamp.companionId.empty() && !result.stamp.SameSession(Session().Stamp())) ||
        (result.audienceRevision != 0 && result.audienceRevision != Session().Audience().revision) ||
        (result.fromAssistant && result.audienceRevision == 0))
        return;
    const QString reasoning = QString::fromStdString(result.reasoning);
    if (!result.text.empty() && !result.spokenAsFragments)
    {
        const QString speaker = result.fromAssistant ? QString::fromStdString(Session().DisplayName()) : QStringLiteral("System");
        const QString body = QString::fromStdString(result.text);
        AppendChat(speaker, body, false, reasoning);
        if (result.fromAssistant)
            CaptureDisplayedAnswer(body, result.stamp, result.audienceRevision);
    }
    else if (result.spokenAsFragments && !reasoning.isEmpty())
    {
        AppendChat(QString(), QString(), false, reasoning);
    }
    if (!result.succeeded && !result.reason.empty())
    {
        AppendActivity("Request stopped: " + QString::fromStdString(result.reason));
        RecordTurnError(result);
    }
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
            AppendChat(entry.second.speaker, entry.second.text, false, entry.second.reasoning);
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
        AppendChat(entry->second.speaker, entry->second.text, false, entry->second.reasoning);
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
    const QString saved = QSettings().value("input/microphoneDevice", QString()).toString();
    microphoneDeviceCombo->clear();
    microphoneDeviceCombo->addItem(QStringLiteral("Default"), QString());
    for (const revia::speech::MicrophoneDevice& device : Session().AvailableMicrophones())
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
    Session().SetMicrophoneDevice(saved.toStdString());

    const revia::speech::MicrophoneSelection resolved = Session().ResolvedMicrophone();
    microphoneDeviceCombo->setToolTip(QString::fromStdString(resolved.report));
    if (resolved.fellBackToDefault)
    {
        microphoneTestResultLabel->setText(QStringLiteral("Microphone error: ") + QString::fromStdString(resolved.report));
        AppendActivity(QStringLiteral("Microphone: ") + QString::fromStdString(resolved.report));
    }
}

void ReviaWindow::RunMicrophoneTest()
{
    if (switchingCompanion || shuttingDown.load())
    {
        return;
    }
    if (deviceWorker.joinable())
    {
        deviceWorker.join();
    }
    testMicrophoneButton->setEnabled(false);
    microphoneTestResultLabel->setText(QStringLiteral("Testing microphone..."));
    // Three seconds of capture is long enough to freeze the window, so the device call
    // runs on a worker and the result is applied back on the GUI thread.
    deviceWorker = std::jthread(
        [this, epoch = sessionUiEpoch.load()]()
        {
            const auto result = RunOrFail<revia::speech::MicrophoneTestResult>([this]() { return Session().TestMicrophone(3, true); },
                [](const std::string& why)
                {
                    revia::speech::MicrophoneTestResult failed;
                    failed.status = "Error";
                    failed.message = why;
                    return failed;
                });
            QMetaObject::invokeMethod(
                this,
                [this, epoch, result]()
                {
                    if (epoch != sessionUiEpoch.load() || shuttingDown.load())
                        return;
                    const QString status = QString::fromStdString(result.status);
                    const QString message = QString::fromStdString(result.message);
                    microphoneTestResultLabel->setText(
                        (result.succeeded ? QStringLiteral("Microphone OK - ") : QStringLiteral("Microphone error: ")) + message);
                    AppendActivity(QStringLiteral("Microphone test (") + status + QStringLiteral("): ") + message);
                    testMicrophoneButton->setEnabled(!shuttingDown.load());
                },
                Qt::QueuedConnection);
        });
}

void ReviaWindow::ToggleListening()
{
    if (switchingCompanion || shuttingDown.load())
    {
        return;
    }

    // Recognition switched off in Settings: the button leads there instead of doing nothing.
    if (microphoneUiState == MicrophoneUi::Unavailable && microphoneDisabledBySetting)
    {
        OpenSettings(QStringLiteral("speechRecognition.enabled"));
        return;
    }
    // Hands-free listens on its own; stopping it means turning hands-free off, which Settings owns.
    if (microphoneUiState == MicrophoneUi::HandsFree)
    {
        if (handsFreeCheck && handsFreeCheck->isChecked() && handsFreeCheck->isEnabled())
            handsFreeCheck->setChecked(false);
        return;
    }

    if (microphoneUiState == MicrophoneUi::Listening)
    {
        listenRequested = false;
        // EndListening hands the capture to whisper.cpp; the Transcribing phase event
        // arrives from the worker and repaints the button.
        ApplyMicrophoneUi(Session().EndListening() ? MicrophoneUi::Transcribing : MicrophoneUi::Ready);
        return;
    }

    if (microphoneUiState != MicrophoneUi::Ready)
    {
        return;
    }
    if (Session().BeginListening())
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
    microphoneActive = microphoneUi == MicrophoneUi::Listening || microphoneUi == MicrophoneUi::Transcribing;

    // Icon, tooltip and accessible name only: the button is a 32px round icon in the composer.
    const auto present = [this](const char* icon, const QString& tip, const bool enabled)
    {
        revia::desktop::theme::BindIcon(microphoneButton, icon);
        microphoneButton->setText(QString());
        microphoneButton->setToolTip(tip);
        microphoneButton->setAccessibleName(tip);
        microphoneButton->setEnabled(enabled);
    };
    switch (microphoneUi)
    {
    case MicrophoneUi::HandsFree:
        present("mic", QStringLiteral("Hands-free is on: click to stop"), true);
        break;
    case MicrophoneUi::Listening:
        present("mic:onAccent", QStringLiteral("Stop listening (Ctrl+Space)"), true);
        break;
    case MicrophoneUi::Transcribing:
        present("mic", QStringLiteral("Transcribing\u2026"), false);
        break;
    case MicrophoneUi::Ready:
        // Recognition reports Ready partway through startup, while the startup thread
        // is still running. Taking a turn then would block the UI joining it, so the
        // button stays disabled until the runtime is actually up.
        present("mic", Session().IsStarted() ? QStringLiteral("Listen (Ctrl+Space)") : QStringLiteral("Available once Revia has started"),
            Session().IsStarted());
        break;
    case MicrophoneUi::Unavailable:
    default:
        // Switched off in Settings, the button stays clickable and leads there.
        if (microphoneDisabledBySetting)
            present("mic-off", QStringLiteral("Microphone is off: turn it on in Settings \u203a Voice"), true);
        else
            present("mic-off", QStringLiteral("Speech recognition is not available."), false);
        break;
    }

    // The pressed-state styling is only meaningful while a press is held, so recolour
    // the button from its logical state instead.
    microphoneButton->setProperty("listening", microphoneUi == MicrophoneUi::Listening);
    Repolish(microphoneButton);
    if (composerHandsFreeChip)
        composerHandsFreeChip->setVisible(microphoneUi == MicrophoneUi::HandsFree);

    RefreshStateBadge();
}

void ReviaWindow::BeginShutdown(const revia::core::ExitReason reason, const std::string& detail)
{
    if (shuttingDown.exchange(true))
    {
        return;
    }
    // Nothing is approved on the way out, and nothing waits on an approval.
    AbandonQuestions();
    // Recorded here rather than after the workers stop, so the reason survives even if
    // shutdown itself is what fails.
    revia::core::ExitReporter::Record(reason, detail);
    // Nothing can start once shutdown begins, and it never unlocks. The open panels are
    // remembered for the next start before they close.
    SetNavigationLocked(true);
    if (panelHost)
    {
        panelHost->SaveOpenSet();
        panelHost->HideAll(false);
    }
    sendButton->setEnabled(false);
    stopButton->setEnabled(false);
    Session().RequestStop();
    studioWorker.request_stop();
    shutdownWorker = std::jthread(
        [this]()
        {
            if (companionSwitchWorker.joinable())
                companionSwitchWorker.join();
            if (studioWorker.joinable())
                studioWorker.join();
            // A Settings write in flight finishes against the session before it stops.
            if (settingsWorker)
                settingsWorker->Stop();
            // Quit whatever happens: a shutdown that threw is still the end of the session,
            // and the exit reason was already recorded above.
            RunOrFail<bool>(
                [this]()
                {
                    Session().Stop();
                    return true;
                },
                [](const std::string&) { return false; });
            QMetaObject::invokeMethod(qApp, []() { QApplication::quit(); }, Qt::QueuedConnection);
        });
}

void ReviaWindow::UseVisibleScreen()
{
    if (switchingCompanion || shuttingDown.load() || Session().IsBusy() || operationRunning.load())
    {
        return;
    }
    const QString instruction = messageInput->toPlainText().trimmed();
    if (instruction.isEmpty())
    {
        QMessageBox::information(
            this, "Screen action instruction", "Write one specific instruction first, such as 'press the Save button'.");
        messageInput->setFocus();
        return;
    }
    const bool approved = QMessageBox::question(this, "Share and resolve screen control",
                              DisplayName() + " will capture only the foreground application window once, locate the requested control "
                              "locally, and require a match to a real Windows UI Automation element. The "
                              "temporary PNG is deleted before any action. There is no coordinate-click "
                              "fallback, and the resulting action still requires normal policy approval. "
                              "Continue?",
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
    if (!approved)
    {
        return;
    }

    if (!ClaimOperationWorker())
        return;
    messageInput->clear();
    lastTurnError.reset();
    userStopRequested = false;
    AppendChat("You", instruction, true);
    showMinimized();
    screenActionButton->setEnabled(false);
    stopButton->setEnabled(true);
    UpdateSendState();
    operationWorker = std::jthread(
        [this, epoch = sessionUiEpoch.load(), request = instruction.toStdString()]()
        {
            const RunningFlag running(operationRunning);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            const auto result = RunOrFail<revia::runtime::SessionResult>([this, &request]() { return Session().ActOnScreen(request); },
                [](const std::string& why)
                {
                    revia::runtime::SessionResult failed;
                    failed.succeeded = false;
                    failed.fromAssistant = false;
                    failed.text = "That screen action could not be completed: " + why;
                    failed.reason = failed.text;
                    return failed;
                });
            QMetaObject::invokeMethod(
                this,
                [this, epoch, result]()
                {
                    if (epoch != sessionUiEpoch.load() || shuttingDown.load())
                        return;
                    showNormal();
                    raise();
                    activateWindow();
                    if (!result.text.empty())
                    {
                        AppendChat(QString::fromStdString(Session().DisplayName()), QString::fromStdString(result.text));
                    }
                    if (!result.succeeded && !result.reason.empty())
                    {
                        AppendActivity("Screen action stopped: " + QString::fromStdString(result.reason));
                        RecordTurnError(result);
                        RenderChat();
                    }
                    screenActionButton->setEnabled(Session().IsVisionAvailable());
                    RefreshStateBadge();
                    messageInput->setFocus();
                },
                Qt::QueuedConnection);
        });
}

void ReviaWindow::DiscoverApplicationPermissions()
{
    if (switchingCompanion || shuttingDown.load() || Session().IsBusy())
    {
        if (capabilityPanel != nullptr)
        {
            capabilityPanel->SetStatus("Wait for the current Revia operation to finish before discovery.", true);
        }
        return;
    }
    showMinimized();
    if (capabilityWorker.joinable())
    {
        capabilityWorker.join();
    }
    capabilityWorker = std::jthread(
        [this, epoch = sessionUiEpoch.load()]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(450));
            const auto inventory = RunOrFail<revia::actions::windows::ApplicationControlInventory>([this]()
                { return Session().DiscoverForegroundApplicationControls(); },
                [](const std::string& why)
                {
                    revia::actions::windows::ApplicationControlInventory failed;
                    failed.reason = "Control discovery failed: " + why;
                    return failed;
                });
            QMetaObject::invokeMethod(
                this,
                [this, epoch, inventory]()
                {
                    if (epoch != sessionUiEpoch.load() || shuttingDown.load())
                        return;
                    showNormal();
                    raise();
                    activateWindow();
                    if (capabilityPanel != nullptr)
                    {
                        capabilityPanel->ShowDiscovery(inventory);
                    }
                },
                Qt::QueuedConnection);
        });
}

void ReviaWindow::RefreshVoiceStudio()
{
    const revia::speech::VoiceStudioSnapshot snapshot = Session().VoiceStudio();
    {
        const QSignalBlocker libraryBlocker(voiceLibraryCombo);
        const QString selected = voiceLibraryCombo->currentData().toString();
        voiceLibraryCombo->clear();
        voiceLibraryCombo->addItem("Select a created voice...", QString());
        for (const auto& preset : snapshot.presets)
        {
            voiceLibraryCombo->addItem(QString::fromStdString(preset.name), QString::fromStdString(preset.id));
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
        voiceStudioStatus->setText(
            snapshot.presets.empty()
                ? "No voices have been created yet. Describe one below and create it."
                : QString::number(static_cast<int>(snapshot.presets.size())) +
                      (snapshot.presets.size() == 1 ? " voice is available. Assign it to a profile in the Profiles window."
                                                    : " voices are available. Assign one to a profile in the Profiles window."));
    }
    if (profilePanel != nullptr)
    {
        profilePanel->Refresh();
    }
}

void ReviaWindow::CreateVoicePreset()
{
    if (switchingCompanion || shuttingDown.load() || voiceOperationRunning.exchange(true))
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
        voiceStudioStatus->setText("Enter a preset name, voice description, and reference line before creating a voice.");
        return;
    }
    for (QPushButton* button : {createVoiceButton, previewVoiceButton, renderVoiceBankButton})
    {
        button->setEnabled(false);
    }
    voiceStudioStatus->setText("Creating the Qwen3-TTS voice. The first run downloads about 4.5 GB of model files; "
                               "progress and hardware choice are written to Activity and Logs/qwen-tts.stdout.log.");
    if (voiceWorker.joinable())
    {
        voiceWorker.join();
    }
    voiceWorker = std::jthread(
        [this, epoch = sessionUiEpoch.load(), name, description, reference, language]()
        {
            const auto result = RunOrFail<revia::speech::VoiceOperationResult>(
                [&]()
                {
                    return Session().CreateVoicePreset(
                        name.toStdString(), description.toStdString(), reference.toStdString(), language.toStdString());
                },
                FailedVoiceOperation);
            QMetaObject::invokeMethod(
                this,
                [this, epoch, result, name]()
                {
                    if (epoch != sessionUiEpoch.load() || shuttingDown.load())
                        return;
                    voiceOperationRunning.store(false);
                    for (QPushButton* button : {createVoiceButton, previewVoiceButton, renderVoiceBankButton})
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
                        revia::desktop::SelectNavigationPage(ui->voiceTab);
                    }
                },
                Qt::QueuedConnection);
        });
}

void ReviaWindow::RenderVoiceBank()
{
    if (switchingCompanion || shuttingDown.load() || voiceOperationRunning.exchange(true))
    {
        return;
    }
    const QString presetId = voiceLibraryCombo->currentData().toString();
    if (presetId.isEmpty())
    {
        voiceOperationRunning.store(false);
        voiceStudioStatus->setText("Select a created voice first.");
        return;
    }
    for (QPushButton* button : {createVoiceButton, previewVoiceButton, renderVoiceBankButton})
    {
        button->setEnabled(false);
    }
    voiceStudioStatus->setText("Rendering this voice's nonverbal sounds. This runs once per voice and the VoiceDesign "
                               "model downloads on first use, so the first time is slow.");
    if (voiceWorker.joinable())
    {
        voiceWorker.join();
    }
    voiceWorker = std::jthread(
        [this, epoch = sessionUiEpoch.load(), presetId]()
        {
            const auto result = RunOrFail<revia::speech::VoiceOperationResult>(
                [&]() { return Session().RenderVoiceBank(presetId.toStdString()); }, FailedVoiceOperation);
            QMetaObject::invokeMethod(
                this,
                [this, epoch, result]()
                {
                    if (epoch != sessionUiEpoch.load() || shuttingDown.load())
                        return;
                    voiceOperationRunning.store(false);
                    for (QPushButton* button : {createVoiceButton, previewVoiceButton, renderVoiceBankButton})
                    {
                        button->setEnabled(true);
                    }
                    voiceStudioStatus->setText(QString::fromStdString(result.message));
                },
                Qt::QueuedConnection);
        });
}

void ReviaWindow::PreviewVoice()
{
    if (switchingCompanion || shuttingDown.load() || voiceOperationRunning.exchange(true))
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
    for (QPushButton* button : {createVoiceButton, previewVoiceButton, renderVoiceBankButton})
    {
        button->setEnabled(false);
    }
    voiceStudioStatus->setText("Generating the preview with Qwen3-TTS. The Base model downloads on first use.");
    if (voiceWorker.joinable())
    {
        voiceWorker.join();
    }
    voiceWorker = std::jthread(
        [this, epoch = sessionUiEpoch.load(), presetId, preview]()
        {
            const auto result = RunOrFail<revia::speech::VoiceOperationResult>(
                [&]() { return Session().PreviewVoice(presetId.toStdString(), preview.toStdString()); }, FailedVoiceOperation);
            QMetaObject::invokeMethod(
                this,
                [this, epoch, result]()
                {
                    if (epoch != sessionUiEpoch.load() || shuttingDown.load())
                        return;
                    voiceOperationRunning.store(false);
                    for (QPushButton* button : {createVoiceButton, previewVoiceButton, renderVoiceBankButton})
                    {
                        button->setEnabled(true);
                    }
                    voiceStudioStatus->setText(QString::fromStdString(result.message));
                },
                Qt::QueuedConnection);
        });
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
    if (!event.stamp.companionId.empty() && !event.stamp.SameSession(Session().Stamp()))
        return;
    const auto currentAudienceRevision = Session().Audience().revision;
    if (event.audienceRevision != 0 && event.audienceRevision != currentAudienceRevision)
        return;
    if ((event.kind == revia::runtime::RuntimeEventKind::AssistantMessage ||
            event.kind == revia::runtime::RuntimeEventKind::ReplyFragment ||
            event.kind == revia::runtime::RuntimeEventKind::InvestigationChecking ||
            event.kind == revia::runtime::RuntimeEventKind::InvestigationFindings) &&
        event.audienceRevision == 0)
        return;
    if (conversationStatus)
    {
        conversationStatus->Observe(event);
        UpdateConversationStrip();
    }
    if (event.kind == revia::runtime::RuntimeEventKind::AgentWorkflow)
    {
        if (agentStudioPanel)
            agentStudioPanel->SetSnapshot(Session().AgentWorkflowSnapshot());
        RefreshStateBadge();
        return;
    }
    RefreshPresenceUi();
    if (event.kind == revia::runtime::RuntimeEventKind::ComponentStatus &&
        (event.component == "Voice health" || event.component == "System cues"))
    {
        const QString summary = voiceHealth->ApplyEvent(event);
        chatVoiceHealth->ApplyEvent(event);
        if (!summary.isEmpty())
        {
            revia::runtime::RuntimeEvent safe;
            safe.kind = event.kind;
            safe.state = event.state;
            safe.occurredAt = event.occurredAt;
            safe.component = event.component;
            safe.phase = event.phase;
            safe.message = summary.toStdString();
            if (pipelinePanel)
                pipelinePanel->Observe(safe);
            AppendComponentActivity(safe, QString::fromStdString(safe.component) + ": " + summary);
        }
        return;
    }
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
        AppendChat(adapter ? QStringLiteral("External") : QStringLiteral("You"), QString::fromStdString(event.message), true);
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::StateChanged)
    {
        UpdateState(event.state, QString::fromStdString(event.message));
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::ReplyFragment)
    {
        // Publish ready sentences immediately, independently of audio generation.
        const QString speaker = QString::fromStdString(Session().DisplayName());
        const QString body = QString::fromStdString(event.message);
        if (event.phase == "Replace")
        {
            ReplaceStreamedReply(event.conversationTurnId, speaker, body);
            CaptureDisplayedAnswer(body, event.stamp, event.audienceRevision, event.conversationTurnId);
            return;
        }
        if (event.conversationTurnId != streamedReplyTurnId)
        {
            streamedReplyTurnId = event.conversationTurnId;
            streamedReplyEntries.clear();
        }
        streamedReplyEntries.push_back(chatEntries.size());
        AppendChat(speaker, body);
        CaptureDisplayedAnswer(body, event.stamp, event.audienceRevision, event.conversationTurnId, true);
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::Proposal)
    {
        // Revia speaking first. Shown with its evidence and dismissible in one action,
        // which is the whole contract: it offers, it never acts.
        const QString message = QString::fromStdString(event.message);
        AppendChat(QString::fromStdString(Session().DisplayName()), message);
        // Drawn as a quiet offer rather than a reply (it still counts as the companion speaking).
        chatEntries.back().proposal = true;
        RenderChat();
        const QString id = QString::fromStdString(event.phase);
        if (event.component == "Improvement")
        {
            // A proven change to her own code. Answered with /improve, not /initiative.
            AppendActivity(QStringLiteral("Improvement #") + id + QStringLiteral(": ") + QString::fromStdString(event.detail) +
                           QStringLiteral(" - /improve show ") + id + QStringLiteral(", /improve accept ") + id +
                           QStringLiteral(" or /improve reject ") + id + QStringLiteral(" <why>"));
            return;
        }
        AppendActivity(QStringLiteral("Proposal ") + id + QStringLiteral(" because ") + QString::fromStdString(event.detail) +
                       QStringLiteral(" - /initiative accept or /initiative dismiss"));
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::SelfInquiry)
    {
        // Shown in the transcript rather than collapsed behind "Thought process". These
        // are the questions Revia put to herself on a hard turn, and the point of them is
        // that the user sees what she was actually wondering before the answer arrives.
        const QString message = QString::fromStdString(event.message);
        const QString phase = QString::fromStdString(event.phase);
        if (event.turnId != 0)
        {
            // Streamed thinking arrives as many events for one turn and updates a single
            // entry in place: a message that extends the shown text replaces it (a
            // snapshot), anything else is appended (a delta), and a new phase of the same
            // turn starts a new paragraph.
            for (std::size_t index = chatEntries.size(); index-- > 0;)
            {
                ChatEntry& entry = chatEntries[index];
                if (entry.kind != EntryKind::SelfInquiry || entry.taskId != event.turnId)
                    continue;
                if (entry.phase != phase && !entry.body.isEmpty())
                    entry.body += QStringLiteral("\n\n") + message;
                else if (message.startsWith(entry.body))
                    entry.body = message;
                else
                    entry.body += message;
                if (!event.detail.empty())
                    entry.reasoning = QString::fromStdString(event.detail);
                entry.phase = phase;
                RenderChat();
                return;
            }
        }
        AppendWorkEntry(EntryKind::SelfInquiry, message, QString::fromStdString(event.detail), event.turnId, 0);
        if (!chatEntries.empty())
            chatEntries.back().phase = phase;
        AppendActivity(QStringLiteral("Self-inquiry: ") + QString::fromStdString(event.detail));
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::InvestigationChecking ||
        event.kind == revia::runtime::RuntimeEventKind::InvestigationFindings)
    {
        // The alternating blocks. `phase` carries the round number so an entry always
        // lands under the task that raised it.
        const bool checking = event.kind == revia::runtime::RuntimeEventKind::InvestigationChecking;
        AppendWorkEntry(checking ? EntryKind::InvestigationChecking : EntryKind::InvestigationFindings,
            QString::fromStdString(event.message), QString::fromStdString(event.detail), event.turnId, event.queueDepth);
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
        affectLabel->setText(QStringLiteral("Affect: ") + QString::fromStdString(revia::runtime::ToString(event.affect)) +
                             QStringLiteral(" ") + QString::number(percent) + QStringLiteral("%"));
        affectLabel->setToolTip(QString::fromStdString(event.message));
        AppendActivity(QStringLiteral("Affect -> ") + QString::fromStdString(revia::runtime::ToString(event.affect)) +
                       QStringLiteral(" (") + QString::number(percent) + QStringLiteral("%): ") + QString::fromStdString(event.message));
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::ComponentStatus)
    {
        if (event.component == "Voice")
        {
            const QString phase = QString::fromStdString(event.phase);
            speechLabel->setText(QStringLiteral("Voice: ") + phase);
            speechLabel->setToolTip(QString::fromStdString(event.message));
            speechActive = phase == QStringLiteral("Queued") || phase == QStringLiteral("Loading") ||
                           phase == QStringLiteral("Designing") || phase == QStringLiteral("Generating") ||
                           phase == QStringLiteral("Generated") || phase == QStringLiteral("Speaking");

            speechPhase = phase;
            RefreshStateBadge();
            if (phase == QStringLiteral("Speaking"))
            {
                // The sync point: audio has started, so show the words now.
                lastSpeakingUtteranceId = event.turnId;
                ReleasePendingSpeechText(event.turnId);
            }
            else if (phase == QStringLiteral("Error") || phase == QStringLiteral("Unavailable") || phase == QStringLiteral("Disabled") ||
                     phase == QStringLiteral("Stopped") || phase == QStringLiteral("Interrupted"))
            {
                // Speech is not coming, or has ended early. Never leave the reply unshown
                // because the audio failed.
                ReleasePendingSpeechText(0);
            }
            // Engine health shows on the Speak replies row; the switch keeps the owner's choice.
            if (settingsDialog)
            {
                if (phase == QStringLiteral("Unavailable") || phase == QStringLiteral("Error"))
                    settingsDialog->SetRowStatus("speech.enabled",
                        QStringLiteral("Voice is unavailable: ") + QString::fromStdString(event.message), QStringLiteral("warning"));
                else if (phase == QStringLiteral("Ready") || phase == QStringLiteral("Speaking") || phase == QStringLiteral("Disabled"))
                    settingsDialog->SetRowStatus("speech.enabled", QString(), QString());
            }
            UpdateSendState();
            QString detail = QStringLiteral("Voice ") + phase + QStringLiteral(": ") + QString::fromStdString(event.message);
            if (event.elapsedMilliseconds >= 0.0)
            {
                detail += QStringLiteral(" (") + QString::number(event.elapsedMilliseconds, 'f', 1) + QStringLiteral("ms)");
            }
            if (event.queueDepth > 0)
            {
                detail += QStringLiteral(" queue=") + QString::number(event.queueDepth);
            }
            AppendActivity(detail);
        }
        else if (event.component == "Microphone")
        {
            const QString phase = QString::fromStdString(event.phase);
            microphoneLabel->setText(QStringLiteral("Mic: ") + phase);
            microphoneLabel->setToolTip(QString::fromStdString(event.message));
            // "Disabled" is the owner's Use the microphone switch; the button then leads to it.
            if (phase != QStringLiteral("Diagnostics") && !phase.startsWith(QStringLiteral("Test")))
                microphoneDisabledBySetting = phase == QStringLiteral("Disabled");
            if (phase == QStringLiteral("Missing") || phase == QStringLiteral("Disabled") || phase == QStringLiteral("Unavailable"))
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
            else if (phase == QStringLiteral("Captured") || phase == QStringLiteral("Transcribing"))
            {
                ApplyMicrophoneUi(MicrophoneUi::Transcribing);
            }
            else if (phase == QStringLiteral("Diagnostics") || phase.startsWith(QStringLiteral("Test")))
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
                // should not lock the button for the rest of the Session().
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
                ShowStatus(reason, 12000);
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
                    event.detail == "hands-free", transcript.isEmpty(), autoSendVoiceCheck->isChecked(), Session().IsBusy()))
                {
                case TranscriptRouting::Ignore:
                    // Deliberately leaves the message box alone: an empty
                    // transcript must not erase something already typed there.
                    AppendActivity(QStringLiteral("Microphone: nothing was recognised in that recording."));
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
            QString detail = QStringLiteral("Microphone ") + phase + QStringLiteral(": ") + QString::fromStdString(event.message);
            if (event.elapsedMilliseconds >= 0.0)
            {
                detail += QStringLiteral(" (") + QString::number(event.elapsedMilliseconds, 'f', 1) + QStringLiteral("ms)");
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
            // An action is running until its Ready or Blocked; while it does, desktop control is
            // live unless the owner already stopped it.
            automationRunning = phase == QStringLiteral("Running");
            RefreshPrivacyIndicators();
            RefreshStateBadge();
            QString detail = QStringLiteral("Automation ") + phase + QStringLiteral(": ") + QString::fromStdString(event.message);
            if (event.elapsedMilliseconds >= 0.0)
            {
                detail += QStringLiteral(" (") + QString::number(event.elapsedMilliseconds, 'f', 1) + QStringLiteral("ms)");
            }
            AppendActivity(detail);
        }
        else if (event.component == "Perception")
        {
            const QString phase = QString::fromStdString(event.phase);
            const QString detail = QString::fromStdString(event.message);
            const bool lifecycle = phase == QStringLiteral("Watching") || phase == QStringLiteral("Paused") ||
                                   phase == QStringLiteral("Off") || phase == QStringLiteral("Error") ||
                                   phase == QStringLiteral("Unavailable");
            if (lifecycle)
            {
                perceptionLabel->setText(
                    QStringLiteral("Watching: ") + (phase == QStringLiteral("Watching") ? QStringLiteral("On") : phase));
                perceptionLabel->setToolTip(detail);
                perceptionLabel->setProperty("watching", phase == QStringLiteral("Watching"));
                Repolish(perceptionLabel);
                perceptionWatching = phase == QStringLiteral("Watching");
                RefreshPrivacyIndicators();
                AppendActivity(QStringLiteral("Perception ") + phase + QStringLiteral(": ") + detail);
            }
            else
            {
                // An observation, not a state change. It goes to the activity feed so
                // there is a visible record of everything perception actually kept.
                AppendActivity(QStringLiteral("Saw (") + phase + QStringLiteral("): ") + detail);
            }
        }
        else if (event.component == "Vision")
        {
            const QString phase = QString::fromStdString(event.phase);
            visionLabel->setText(QStringLiteral("Vision: ") + phase);
            visionLabel->setToolTip(QString::fromStdString(event.message));
            // Continuous awareness has no event of its own: its passes report Watching and Aware,
            // and the vision status names it when it is on. A one-off look for a question ("Ready"
            // with another message) leaves the indicator as it was.
            const QString visionMessage = QString::fromStdString(event.message);
            if (phase == QStringLiteral("Watching") || phase == QStringLiteral("Aware") ||
                (phase == QStringLiteral("Ready") && visionMessage.startsWith(QStringLiteral("Continuous"))))
                screenAwarenessRunning = true;
            else if (phase == QStringLiteral("Disabled") || phase == QStringLiteral("Error") ||
                     (phase == QStringLiteral("Ready") && visionMessage.startsWith(QStringLiteral("Local screen vision is ready"))))
                screenAwarenessRunning = false;
            RefreshPrivacyIndicators();
            screenActionButton->setEnabled(phase != QStringLiteral("Disabled") && phase != QStringLiteral("Unavailable") &&
                                           phase != QStringLiteral("Error") && phase != QStringLiteral("Capturing") &&
                                           phase != QStringLiteral("Analyzing") && phase != QStringLiteral("Grounding") &&
                                           phase != QStringLiteral("Resolving") && !Session().IsBusy());
            QString detail = QStringLiteral("Vision ") + phase + QStringLiteral(": ") + QString::fromStdString(event.message);
            if (event.elapsedMilliseconds >= 0.0)
            {
                detail += QStringLiteral(" (") + QString::number(event.elapsedMilliseconds, 'f', 1) + QStringLiteral("ms)");
            }
            AppendComponentActivity(event, detail);
        }
        else
        {
            if (event.component == "Camera")
            {
                cameraCapturing = event.phase == "Capturing";
                RefreshPrivacyIndicators();
            }
            else if (event.component == "Preferences" && event.phase == "Changed")
            {
                // /bargein, /set and Settings itself: the dialog re-reads once the burst settles.
                SchedulePreferenceRefresh();
            }
            if ((event.component == "Internet" || event.component == "Permissions" || event.component == "Browser" ||
                    event.component == "Commands" || event.component == "DesktopControl" ||
                    (event.component == "Camera" && (event.phase == "Ready" || event.phase == "Disabled"))) &&
                capabilityRefreshTimer)
            {
                // /internet and every other capability change: the permission switches in Settings
                // re-read the capability file, so none of them shows (or writes back) a stale value.
                capabilityRefreshTimer->start();
            }
            QString detail = QString::fromStdString(event.component) + QStringLiteral(" ") + QString::fromStdString(event.phase) +
                             QStringLiteral(": ") + QString::fromStdString(event.message);
            if (event.elapsedMilliseconds >= 0.0)
            {
                detail += QStringLiteral(" (") + QString::number(event.elapsedMilliseconds, 'f', 1) + QStringLiteral("ms)");
            }
            if (event.queueDepth > 0)
            {
                detail += QStringLiteral(" queue=") + QString::number(event.queueDepth);
            }
            AppendComponentActivity(event, detail);
        }
        return;
    }
    if (event.kind == revia::runtime::RuntimeEventKind::AssistantMessage)
    {
        // A reply to merged voice input. Nobody is waiting on a return value for these,
        // so they arrive here instead. Show them as soon as they are ready too.
        const QString speaker = QString::fromStdString(Session().DisplayName());
        const QString body = QString::fromStdString(event.message);
        const QString reasoning = QString::fromStdString(event.detail);
        AppendChat(speaker, body, false, reasoning);
        CaptureDisplayedAnswer(body, event.stamp, event.audienceRevision, event.turnId);
        return;
    }

    const QString message = QString::fromStdString(event.message);
    const QString timestamped = message.startsWith('[') ? message : '[' + EventTime(event) + "] " + message;
    AppendActivity(timestamped,
        event.kind == revia::runtime::RuntimeEventKind::Error
            ? ActivitySeverity::Error
            : (event.kind == revia::runtime::RuntimeEventKind::Warning ? ActivitySeverity::Warning : ActivitySeverity::Automatic));
}

void ReviaWindow::CaptureDisplayedAnswer(const QString& text, const revia::runtime::RuntimeStamp& origin,
    const std::uint64_t audienceRevision, const std::uint64_t turnId, const bool fragment)
{
    if (text.isEmpty())
        return;
    const auto stamp = origin.companionId.empty() ? Session().Stamp() : origin;
    if (!stamp.SameSession(Session().Stamp()) || audienceRevision == 0 || audienceRevision != Session().Audience().revision)
        return;
    if (fragment && turnId != 0 && turnId == displayedAnswerTurnId && displayedAnswerTarget.origin.SameSession(stamp) &&
        displayedAnswerTarget.audienceRevision == audienceRevision)
        latestDisplayedAnswer += "\n\n" + text;
    else
        latestDisplayedAnswer = text;
    displayedAnswerTurnId = turnId;
    displayedAnswerTarget = {};
    displayedAnswerTarget.origin = stamp;
    displayedAnswerTarget.audienceRevision = audienceRevision;
    displayedAnswerTarget.targetId = "displayed-reply:" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    displayedAnswerTarget.targetDigest = revia::audit::ContentDigest(latestDisplayedAnswer.toStdString());
    displayedAnswerTarget.dependencies = {
        {"displayed-result", displayedAnswerTarget.targetDigest}, {"answer-contract", "authored-answer-obligation-v1"}};
    conversationStatus->ClearOwnerOutcomeIfMatching(ExpiredAnswerReviewOutcome);
    const bool reviewWasAvailable = conversationStatus->ReviewButton()->isEnabled();
    conversationStatus->SetReviewAvailable(true);
    // The newest reply's Review link appears with the first reply that can be reviewed.
    if (!reviewWasAvailable)
        RenderChat();
}

void ReviaWindow::ReviewDisplayedAnswer()
{
    if (switchingCompanion || shuttingDown.load() || latestDisplayedAnswer.isEmpty())
        return;
    if (!displayedAnswerTarget.origin.SameSession(Session().Stamp()) || displayedAnswerTarget.audienceRevision == 0 ||
        displayedAnswerTarget.audienceRevision != Session().Audience().revision)
    {
        conversationStatus->SetReviewAvailable(false);
        conversationStatus->SetOwnerOutcome(ExpiredAnswerReviewOutcome);
        return;
    }
    if (answerFeedbackDialog)
    {
        answerFeedbackDialog->raise();
        answerFeedbackDialog->activateWindow();
        return;
    }
    answerFeedbackDialog = new AnswerFeedbackDialog(displayedAnswerTarget, latestDisplayedAnswer, this);
    const QPointer<AnswerFeedbackDialog> dialog = answerFeedbackDialog;
    connect(dialog->RecordButton(), &QPushButton::clicked, this,
        [this, dialog]
        {
            if (!dialog)
                return;
            if (switchingCompanion || shuttingDown.load() || studioOperationRunning.load())
            {
                dialog->SetOutcome(false, "Another companion operation is active. Record the judgment after it finishes.");
                return;
            }
            revia::learning::QualityFeedback feedback;
            std::string error;
            if (!dialog->CaptureFeedback(feedback, error))
            {
                dialog->SetOutcome(false, error);
                return;
            }
            dialog->SetBusy(true);
            RunStudioOperation(
                [feedback](revia::runtime::ReviaSession& current, const std::stop_token stopToken, std::string& message)
                {
                    std::string taskId, recordId;
                    if (!current.RecordQualityFeedback(feedback, taskId, recordId, message, stopToken))
                        return false;
                    message = "Pending quality task " + taskId + "; private lesson " + recordId +
                              ". Review the candidate in the Learning window. No lesson has been accepted.";
                    return true;
                },
                [this, dialog](const bool success, const std::string& message)
                {
                    if (dialog)
                    {
                        dialog->SetBusy(false);
                        dialog->SetOutcome(success, message);
                    }
                    conversationStatus->SetOwnerOutcome(QString::fromStdString(message));
                });
        });
    dialog->show();
}

void ReviaWindow::ResetConversationPresentation()
{
    if (answerFeedbackDialog)
        delete answerFeedbackDialog.data();
    latestDisplayedAnswer.clear();
    displayedAnswerTarget = {};
    displayedAnswerTurnId = 0;
    streamedReplyTurnId = 0;
    streamedReplyEntries.clear();
    viewingArchivedChat.clear();
    if (archivedChatBanner)
        archivedChatBanner->hide();
    editingLastTurn = false;
    chatTurnInFlight = false;
    lastTurnError.reset();
    userStopRequested = false;
    transcriptPinned = true;
    if (jumpToLatest)
        jumpToLatest->hide();
    if (composer)
        composer->ClearContext();
    composerContextKind = ComposerContextKind::None;
    conversationStatus->ResetForCompanion();
    for (const char* name : {"conversationPageScroll", "memoryPageScroll"})
    {
        if (auto* scroll = findChild<QScrollArea*>(name))
            scroll->verticalScrollBar()->setValue(0);
    }
}

void ReviaWindow::ConfigureMemoryRevisionControls()
{
    const QPointer<MemoryPanel> panel = memoryPanel;
    memoryPanel->SetRevisionSubmitter(
        [this, panel](const revia::memory::MemoryRevisionRequest& request)
        {
            if (switchingCompanion || shuttingDown.load() || studioOperationRunning.load())
                return false;
            if (panel)
                panel->SetRevisionBusy(true);
            RunStudioOperation(
                [request](revia::runtime::ReviaSession& current, const std::stop_token stopToken, std::string& message)
                {
                    revia::memory::MemoryRevisionReceipt receipt;
                    if (!current.ReviseMemoryOwnerRequested(request, receipt, message, stopToken))
                        return false;
                    message = "Revision receipt " + receipt.requestId + ": original " + receipt.originalId + "; replacement " +
                              receipt.revisedId + ". The original record remains in history.";
                    return true;
                },
                [panel](const bool success, const std::string& message)
                {
                    if (panel)
                    {
                        panel->SetRevisionBusy(false);
                        panel->SetRevisionOutcome(success, message);
                        panel->Refresh();
                    }
                });
            return true;
        });
}

void ReviaWindow::UpdateState(const revia::runtime::RuntimeState newState, const QString& detail)
{
    lastRuntimeState = newState;
    lastRuntimeDetail = detail;
    RefreshStateBadge();
}

void ReviaWindow::RefreshStateBadge()
{
    const revia::runtime::RuntimeState newState = lastRuntimeState;
    const QString& detail = lastRuntimeDetail;
    StatePresentation shown = PresentState(newState, DesktopControlActive());
    // Speech outlives the turn that produced it, and the microphone works while the runtime
    // idles: while either is busy, say so rather than reporting Ready at someone who is about to
    // be spoken to.
    if (newState == revia::runtime::RuntimeState::Idle)
    {
        if (microphoneUiState == MicrophoneUi::Listening)
            shown = {"listening", QStringLiteral("Listening")};
        else if (microphoneUiState == MicrophoneUi::Transcribing)
            shown = {"listening", QStringLiteral("Transcribing")};
        else if (speechActive)
            shown = {"speaking", speechPhase == QStringLiteral("Speaking") ? QStringLiteral("Speaking") : QStringLiteral("Preparing voice")};
    }
    if (stateLabel != nullptr)
    {
        stateLabel->setText(shown.label);
        stateDot->setProperty("state", QString::fromLatin1(shown.state));
        Repolish(stateDot);
        statePill->setAccessibleName(QStringLiteral("Status: ") + shown.label);
        statePill->setToolTip(newState == revia::runtime::RuntimeState::WaitingForConfirmation
                                  ? QStringLiteral("An action is waiting for your OK: click to see it")
                                  : QStringLiteral("Status: click for details"));
        // The pill is a button holding a dot and a label; it is as wide as they are, and 28px tall
        // so its 14px radius draws round ends (Qt squares a radius over half the height).
        if (QLayout* pillLayout = statePill->layout())
        {
            pillLayout->invalidate();
            const QSize hint = pillLayout->sizeHint();
            statePill->setFixedSize(hint.width(), std::max(28, hint.height()));
        }
    }
    const QString cleanDetail = detail.trimmed().compare(shown.label, Qt::CaseInsensitive) == 0 ? QString() : detail.trimmed();
    if (stateDetailLabel != nullptr)
    {
        stateDetailLabel->setToolTip(cleanDetail);
        stateDetailLabel->setText(stateDetailLabel->fontMetrics().elidedText(cleanDetail, Qt::ElideRight, 280));
        stateDetailLabel->setVisible(!cleanDetail.isEmpty() && (chatColumn == nullptr || chatColumn->width() >= 640));
    }
    if (runtimeDetailsButton)
        runtimeDetailsButton->setVisible(newState == revia::runtime::RuntimeState::Error);
    if (trayIcon)
    {
        trayIcon->setToolTip("Revia - " + shown.label);
    }

    const bool cancellable = newState == revia::runtime::RuntimeState::Starting || newState == revia::runtime::RuntimeState::Thinking ||
                             newState == revia::runtime::RuntimeState::Responding || newState == revia::runtime::RuntimeState::Acting;
    // Foreground commands can change the badge while a background task continues.
    // That task remains cancellable regardless of the foreground state.
    stopButton->setEnabled(cancellable || studioOperationRunning.load() || Session().HasRunningTask() || speechActive || microphoneActive);
    if (composer)
    {
        QString voice = QStringLiteral("idle");
        if (microphoneUiState == MicrophoneUi::Listening)
            voice = QStringLiteral("listening");
        else if (microphoneUiState == MicrophoneUi::Transcribing)
            voice = QStringLiteral("transcribing");
        else if (microphoneUiState == MicrophoneUi::HandsFree)
            voice = QStringLiteral("handsfree");
        else if (speechActive && speechPhase == QStringLiteral("Speaking"))
            voice = QStringLiteral("speaking");
        composer->SetVoiceState(voice);
        composer->SetPlaceholder(newState == revia::runtime::RuntimeState::Starting
                                     ? QStringLiteral("You can type now; send once %1 is ready").arg(DisplayName())
                                     : QStringLiteral("Message %1").arg(DisplayName()));
    }
    UpdateSendState();
    RefreshPrivacyIndicators();
    UpdateConversationStrip();
    // The greeting and the starting and offline notes in an empty chat follow the state.
    if (chatEntries.empty() && viewingArchivedChat.empty() && chatHistory != nullptr && lastRenderedState != newState)
        RenderChat();
}

void ReviaWindow::UpdateSendState()
{
    if (sendButton == nullptr || stopButton == nullptr)
        return;
    const bool turnBusy = operationRunning.load() || Session().IsBusy();
    const bool pending = !messageInput->toPlainText().trimmed().isEmpty() || (attachmentTray != nullptr && !attachmentTray->IsEmpty());
    sendButton->setEnabled(Session().IsStarted() && !turnBusy && !shuttingDown.load() && !switchingCompanion && pending);
    // Stop takes the send slot while a turn, a task or a studio operation runs; while only speech
    // or the microphone runs it waits beside Send, so the next message can still go.
    const revia::runtime::RuntimeState state = lastRuntimeState;
    // Starting is not a turn: there is no reply to stop, and Send stays in its slot (disabled until
    // Revia has started) beside the outlined Stop that cancels the startup.
    const bool turnRunning = turnBusy || chatTurnInFlight || studioOperationRunning.load() || Session().HasRunningTask() ||
                             state == revia::runtime::RuntimeState::Thinking || state == revia::runtime::RuntimeState::Responding ||
                             state == revia::runtime::RuntimeState::Acting;
    const bool stopEnabled = stopButton->isEnabled();
    const bool primary = stopEnabled && turnRunning;
    if (stopButton->property("primary").toBool() != primary || !stopButton->property("primary").isValid())
    {
        stopButton->setProperty("primary", primary);
        Repolish(stopButton);
        revia::desktop::theme::BindIcon(stopButton, primary ? "square:onAccent" : "square");
    }
    QString stopName = QStringLiteral("Stop reply");
    if (studioOperationRunning.load() || Session().HasRunningTask())
        stopName = QStringLiteral("Stop task");
    else if (!turnRunning && state == revia::runtime::RuntimeState::Starting)
        stopName = QStringLiteral("Cancel startup");
    else if (!turnRunning && speechActive)
        stopName = QStringLiteral("Stop speaking");
    else if (!turnRunning && microphoneActive)
        stopName = QStringLiteral("Stop listening");
    stopButton->setToolTip(primary ? stopName + QStringLiteral(" (Esc)") : stopName);
    stopButton->setAccessibleName(stopName);
    stopButton->setVisible(stopEnabled);
    sendButton->setVisible(!primary);
    // "Still working" and "queued" last exactly as long as the turn they describe.
    if (!turnRunning && composer &&
        (composerContextKind == ComposerContextKind::StillWorking || composerContextKind == ComposerContextKind::Queued))
    {
        composer->ClearContext();
        composerContextKind = ComposerContextKind::None;
    }
}

void ReviaWindow::AppendChat(const QString& speaker, const QString& message, const bool userMessage, const QString& reasoning)
{
    ChatEntry entry;
    entry.speaker = speaker;
    entry.body = message;
    entry.reasoning = reasoning;
    entry.userMessage = userMessage;
    entry.markdown = !userMessage && !speaker.isEmpty() && speaker == QString::fromStdString(Session().DisplayName());
    chatEntries.push_back(std::move(entry));
    TrimTranscript();
    RenderChat();
}

void ReviaWindow::TrimTranscript()
{
    if (chatEntries.size() <= MaximumChatEntries)
        return;
    const std::size_t dropped = chatEntries.size() - MaximumChatEntries;
    chatEntries.erase(chatEntries.begin(), chatEntries.begin() + static_cast<std::ptrdiff_t>(dropped));
    chatEntriesDropped += dropped;
    std::vector<std::size_t> kept;
    for (const std::size_t index : streamedReplyEntries)
        if (index >= dropped)
            kept.push_back(index - dropped);
    streamedReplyEntries = std::move(kept);
    // The failed-turn row keeps its place after the entry it followed, or goes with it.
    if (lastTurnError)
    {
        if (lastTurnError->afterEntry > dropped)
            lastTurnError->afterEntry -= dropped;
        else
            lastTurnError.reset();
    }
}

bool ReviaWindow::EndsMessage(const std::size_t index) const
{
    const ChatEntry& entry = chatEntries[index];
    if (entry.kind != EntryKind::Message || entry.userMessage || entry.speaker.isEmpty() || entry.body.isEmpty())
        return false;
    for (std::size_t next = index + 1; next < chatEntries.size(); ++next)
    {
        const ChatEntry& following = chatEntries[next];
        // A blanked fragment or a reasoning-only line belongs to the message around it.
        if (following.kind == EntryKind::Message && following.speaker.isEmpty() && following.body.isEmpty())
            continue;
        return !(following.kind == EntryKind::Message && !following.userMessage && following.speaker == entry.speaker &&
                 !following.body.isEmpty());
    }
    return true;
}

QString ReviaWindow::MessageText(const std::size_t last) const
{
    const QString speaker = chatEntries[last].speaker;
    QStringList parts;
    for (std::size_t index = last + 1; index-- > 0;)
    {
        const ChatEntry& entry = chatEntries[index];
        if (entry.kind == EntryKind::Message && entry.speaker.isEmpty() && entry.body.isEmpty())
            continue;
        if (entry.kind != EntryKind::Message || entry.userMessage || entry.speaker != speaker)
            break;
        parts.prepend(entry.body);
    }
    return parts.join(QStringLiteral("\n\n"));
}

const QString& ReviaWindow::RenderedBody(ChatEntry& entry)
{
    if (entry.renderedBody.isNull() || entry.renderedSource != entry.body)
    {
        entry.renderedSource = entry.body;
        entry.renderedBody = entry.markdown ? revia::desktop::RenderUntrustedMarkdown(entry.body) : HtmlParagraph(entry.body);
    }
    return entry.renderedBody;
}

void ReviaWindow::ReplaceStreamedReply(const std::uint64_t conversationTurnId, const QString& speaker, const QString& body)
{
    // The first fragment keeps its place and takes the approved text; the rest stop
    // drawing. Entries are blanked rather than removed so every later index stays valid.
    bool placed = false;
    if (conversationTurnId != 0 && conversationTurnId == streamedReplyTurnId)
    {
        for (const std::size_t index : streamedReplyEntries)
        {
            if (index >= chatEntries.size())
                continue;
            ChatEntry& entry = chatEntries[index];
            if (!placed)
            {
                entry.body = body;
                placed = true;
            }
            else
            {
                entry.speaker.clear();
                entry.body.clear();
            }
        }
    }
    streamedReplyEntries.clear();
    if (placed)
        RenderChat();
    else
        AppendChat(speaker, body);
}

void ReviaWindow::AppendWorkEntry(const EntryKind kind, const QString& body, const QString& detail, const quint64 taskId, const int round)
{
    // Recorded whether or not it is displayed. Hiding the panel must not become a way of
    // losing the record, and it must not touch whether the work happened.
    ChatEntry entry;
    entry.body = body;
    entry.reasoning = detail;
    entry.kind = kind;
    entry.taskId = taskId;
    entry.round = round;
    chatEntries.push_back(std::move(entry));
    TrimTranscript();
    RenderChat();
}

QString ReviaWindow::TranscriptCss() const
{
    // The theme's classes, with the owner's chat text size (Appearance › Chat text size) for the
    // replies and the user's bubble.
    return revia::desktop::theme::DocumentCss() +
           QStringLiteral(".assistant, .userBubble, .userBubble td { font-size: %1px; }\n"
                          ".assistant p, .assistant li { line-height: 160%; }\n")
               .arg(chatTextSize);
}

QString ReviaWindow::UserBubbleHtml(const QString& text) const
{
    // Right-aligned and only as wide as it needs to be, up to 80% of the measure. Qt ignores a
    // percentage width on a table nested in a cell, so an empty 20% cell holds the bubble back.
    return QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\" style=\"margin-top:24px;\"><tr><td width=\"20%\"></td>"
                          "<td width=\"80%\" align=\"right\">"
                          "<table class=\"userBubble\" cellspacing=\"0\" cellpadding=\"12\" bgcolor=\"%1\"><tr><td>%2</td></tr></table>"
                          "</td></tr></table>")
        .arg(revia::desktop::theme::Current().userBubble.name(), HtmlParagraph(text));
}

QString ReviaWindow::EmptyStateHtml() const
{
    using revia::runtime::RuntimeState;
    const QString name = DisplayName().toHtmlEscaped();
    // ApplyChatMeasure moves it a third of the way down the transcript.
    const QString open = QStringLiteral("<p class=\"emptyState\" align=\"center\" style=\"margin-bottom:8px;\">");
    const bool modelDown = lastRuntimeState == RuntimeState::Offline ||
                           (lastRuntimeState == RuntimeState::Error && lastRuntimeDetail.contains(QStringLiteral("unavailable"), Qt::CaseInsensitive));
    if (lastRuntimeState == RuntimeState::Starting)
        return open + name + QStringLiteral(" is starting…</p><p class=\"secondary\" align=\"center\">Loading models. You can type now "
                                            "and send once ") +
               name + QStringLiteral(" is ready.</p>");
    if (modelDown)
        return open + QStringLiteral("The language model isn't running</p><p class=\"secondary\" align=\"center\">"
                                     "<a href=\"panel:activity\">Open Activity</a> &middot; <a href=\"panel:resources\">Open Resources</a></p>");
    const int hour = QTime::currentTime().hour();
    const QString greeting = hour >= 5 && hour < 12 ? QStringLiteral("Good morning")
                             : hour >= 12 && hour < 18 ? QStringLiteral("Good afternoon")
                                                       : QStringLiteral("Good evening");
    return open + greeting + QStringLiteral("</p><p class=\"secondary\" align=\"center\">") + name + QStringLiteral(" is ready</p>");
}

std::optional<std::size_t> ReviaWindow::LatestFinishedReply() const
{
    const QString assistant = DisplayName();
    for (std::size_t index = chatEntries.size(); index-- > 0;)
    {
        const ChatEntry& entry = chatEntries[index];
        if (entry.kind != EntryKind::Message || entry.userMessage || entry.proposal || entry.speaker != assistant || entry.body.isEmpty())
            continue;
        return EndsMessage(index) ? std::optional<std::size_t>(index) : std::nullopt;
    }
    return std::nullopt;
}

void ReviaWindow::RenderChat()
{
    // A saved chat stays on screen, scrolled where the reader left it; the live entries
    // keep collecting and are drawn again on the way back.
    if (!viewingArchivedChat.empty())
        return;
    lastRenderedState = lastRuntimeState;
    QScrollBar* bar = chatHistory->verticalScrollBar();
    const int previousValue = bar->value();
    const bool pinned = transcriptPinned || bar->maximum() - previousValue <= 48;
    const revia::desktop::theme::Tokens& tokens = revia::desktop::theme::Current();
    const QString name = DisplayName();
    const QString assistant = name;
    // Rebuilt in full rather than appended, because expanding one entry changes the
    // document and QTextBrowser has no way to toggle a region in place.
    QString html;
    html.reserve(4096);
    const std::optional<std::size_t> editable = chatTurnInFlight ? std::nullopt : LastUserEntry();
    const std::optional<std::size_t> regenerable = chatTurnInFlight ? std::nullopt : RegenerableReply();
    const std::optional<std::size_t> actionable = chatTurnInFlight ? std::nullopt : LatestFinishedReply();
    const bool reviewable = conversationStatus != nullptr && conversationStatus->ReviewButton()->isEnabled();
    const auto errorRow = [this]()
    {
        if (lastTurnError->stoppedByUser)
            return QStringLiteral("<p class=\"activityRow\">Stopped</p>");
        return QStringLiteral("<p class=\"activityRow error\">⚠ Couldn't finish this reply: %1 &middot; <a href=\"retry:\">Retry</a> "
                              "&middot; <a href=\"panel:activity\">Details</a></p>")
            .arg(HtmlParagraph(lastTurnError->reason));
    };
    const auto expander = [](const bool expanded) { return expanded ? QStringLiteral("&#9662;") : QStringLiteral("&#9656;"); };
    QString lastAssistantSpeaker;
    if (chatEntries.empty() && !lastTurnError)
        html = EmptyStateHtml();
    for (std::size_t index = 0; index < chatEntries.size(); ++index)
    {
        if (lastTurnError && lastTurnError->afterEntry == index)
            html += errorRow();
        ChatEntry& entry = chatEntries[index];
        const bool working = entry.kind != EntryKind::Message;
        const bool activity = !working && !entry.userMessage && (entry.proposal || entry.speaker == QStringLiteral("Memory"));
        // Thinking, checking, memory and proposals are Revia working, not Revia replying; the
        // owner can hide them. A hidden entry is still recorded and still counts.
        if ((working || activity) && !showWorkSummaries)
            continue;
        // Each entry starts with an anchor, so a right-click can tell which message it is on.
        html += QStringLiteral("<a name=\"m%1\"></a>").arg(index);

        if (working)
        {
            QString label;
            switch (entry.kind)
            {
            case EntryKind::InvestigationChecking:
                label = name.toHtmlEscaped() + QStringLiteral(" is checking");
                break;
            case EntryKind::InvestigationFindings:
                label = QStringLiteral("Findings");
                break;
            default:
                label = name.toHtmlEscaped() + QStringLiteral(" is thinking");
                break;
            }
            if (entry.round > 0)
                label += QStringLiteral(" (round ") + QString::number(entry.round) + QStringLiteral(")");
            html += QStringLiteral("<p class=\"work\" style=\"margin-top:12px; margin-bottom:4px;\"><a href=\"thought:%1\">%2 %3</a></p>")
                        .arg(QString::number(index), expander(entry.expanded), label);
            if (entry.expanded && (!entry.body.isEmpty() || !entry.reasoning.isEmpty()))
            {
                QString evidence = HtmlParagraph(entry.body);
                if (!entry.reasoning.isEmpty())
                    evidence += (evidence.isEmpty() ? QString() : QStringLiteral("<br><br>")) + HtmlParagraph(entry.reasoning);
                html += QStringLiteral("<table class=\"evidence\" width=\"100%\" cellspacing=\"0\" cellpadding=\"10\" bgcolor=\"%1\"><tr><td>%2</td></tr></table>")
                            .arg(tokens.surfaceAlt.name(), evidence);
            }
            continue;
        }
        if (activity)
        {
            html += QStringLiteral("<p class=\"activityRow\" style=\"margin-top:8px;\">%1 %2</p>")
                        .arg(entry.proposal ? QStringLiteral("✦") : QStringLiteral("◇"), HtmlParagraph(entry.body));
            continue;
        }

        if (entry.userMessage)
        {
            // An adapter's line names who said it; the owner's own turns need no label.
            if (entry.speaker != QStringLiteral("You") && !entry.speaker.isEmpty())
                html += QStringLiteral("<p class=\"speaker\" align=\"right\">%1</p>").arg(entry.speaker.toHtmlEscaped());
            if (!entry.body.isEmpty())
                html += UserBubbleHtml(entry.body);
        }
        else
        {
            // One speaker line where it helps: the first reply, a change of speaker, someone other
            // than the companion, and System output. Streamed sentences share one.
            const bool system = entry.speaker == QStringLiteral("System");
            if (!entry.speaker.isEmpty() && (system || entry.speaker != assistant || entry.speaker != lastAssistantSpeaker))
            {
                if (system)
                    html += QStringLiteral("<p class=\"speaker\">System</p>");
                else
                    html += QStringLiteral("<p class=\"speaker\"><span style=\"color:%1;\">&#9679;</span>&nbsp;%2</p>")
                                .arg(tokens.accent.name(), entry.speaker.toHtmlEscaped());
            }
            if (!entry.speaker.isEmpty())
                lastAssistantSpeaker = system ? QString() : entry.speaker;
            if (!entry.body.isEmpty() && entry.markdown)
                // Block-level HTML from the Markdown conversion, so it cannot sit inside a <p>.
                html += QStringLiteral("<div class=\"assistant\">") + RenderedBody(entry) + QStringLiteral("</div>");
            else if (!entry.body.isEmpty())
                html += QStringLiteral("<p class=\"assistant\">%1</p>").arg(RenderedBody(entry));
        }

        if (!entry.attachments.empty())
        {
            // Names and sizes only; a picture's thumbnail is an image resource of the
            // document, registered under the attachment's id.
            QString files;
            for (const AttachmentChip& chip : entry.attachments)
            {
                if (!chip.thumbnail.isNull())
                {
                    const QUrl source(QStringLiteral("revia-attachment:") + QString::fromStdString(chip.id));
                    chatHistory->document()->addResource(QTextDocument::ImageResource, source, chip.thumbnail);
                    files += QStringLiteral("<img src=\"%1\" height=\"48\"> ").arg(source.toString());
                }
                files += QStringLiteral("%1 (%2)&nbsp;&nbsp;").arg(chip.name.toHtmlEscaped(), chip.size.toHtmlEscaped());
            }
            html += QStringLiteral("<p class=\"attachment\" align=\"%1\">Attached: %2</p>")
                        .arg(entry.userMessage ? QStringLiteral("right") : QStringLiteral("left"), files);
        }

        // Copy, Regenerate and Review sit under the newest finished reply only; older replies are
        // copied from the transcript's context menu.
        if (actionable == index)
        {
            QString links = QStringLiteral("<a href=\"copy:%1\">Copy</a>").arg(QString::number(index));
            if (regenerable == index)
                links += QStringLiteral(" &middot; <a href=\"regen:%1\">Regenerate</a>").arg(QString::number(index));
            if (reviewable)
                links += QStringLiteral(" &middot; <a href=\"review:%1\">Review</a>").arg(QString::number(index));
            html += QStringLiteral("<p class=\"actions\">%1</p>").arg(links);
        }
        if (editable == index)
            html += QStringLiteral("<p class=\"actions\" align=\"right\"><a href=\"edit:%1\">Edit</a></p>").arg(QString::number(index));

        if (!entry.reasoning.isEmpty() && showWorkSummaries)
        {
            // The anchor carries the entry index; anchorClicked toggles it and re-renders.
            html += QStringLiteral("<p class=\"thought\" align=\"%1\"><a href=\"thought:%2\">%3 Thought process</a></p>")
                        .arg(entry.userMessage ? QStringLiteral("right") : QStringLiteral("left"), QString::number(index), expander(entry.expanded));
            if (entry.expanded)
                html += QStringLiteral("<table class=\"evidence\" width=\"100%\" cellspacing=\"0\" cellpadding=\"10\" bgcolor=\"%1\"><tr><td>%2</td></tr></table>")
                            .arg(tokens.surfaceAlt.name(), HtmlParagraph(entry.reasoning));
        }
    }
    if (lastTurnError && lastTurnError->afterEntry >= chatEntries.size())
        html += errorRow();
    // While a reply is on its way and none of it has arrived yet.
    if (chatTurnInFlight)
    {
        const std::optional<std::size_t> user = LastUserEntry();
        bool answered = false;
        for (std::size_t index = user ? *user + 1 : 0; index < chatEntries.size() && !answered; ++index)
            answered = !chatEntries[index].userMessage && chatEntries[index].kind == EntryKind::Message && chatEntries[index].speaker == assistant &&
                       !chatEntries[index].body.isEmpty();
        if (!answered)
            html += QStringLiteral("<p class=\"replying\" style=\"margin-top:24px;\">%1 is replying…</p>").arg(name.toHtmlEscaped());
    }

    const bool changed = html != lastTranscriptHtml;
    lastTranscriptHtml = html;
    renderingTranscript = true;
    chatHistory->document()->setDefaultStyleSheet(TranscriptCss());
    chatHistory->setHtml(html);
    ApplyChatMeasure();
    // Lays the document out now, so the scroll range below is the new one.
    (void)chatHistory->document()->documentLayout()->documentSize();
    if (renderKeepsPosition || !pinned)
    {
        bar->setValue(previousValue);
        if (!renderKeepsPosition && changed && jumpToLatest != nullptr && chatColumn != nullptr && chatColumn->isVisible())
        {
            jumpToLatest->show();
            jumpToLatest->raise();
        }
    }
    else
    {
        bar->setValue(bar->maximum());
    }
    renderingTranscript = false;
}

std::optional<std::size_t> ReviaWindow::LastUserEntry() const
{
    for (std::size_t index = chatEntries.size(); index-- > 0;)
    {
        const ChatEntry& entry = chatEntries[index];
        if (entry.kind == EntryKind::Message && entry.userMessage)
        {
            // Typed lines only; an external adapter's line is not the owner's to edit.
            if (entry.speaker == QStringLiteral("You"))
                return index;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> ReviaWindow::RegenerableReply() const
{
    const std::optional<std::size_t> user = LastUserEntry();
    if (!user)
        return std::nullopt;
    const QString assistant = QString::fromStdString(Session().DisplayName());
    // The newest message with a speaker must be Revia's reply to that line: a System
    // result (a command) or a Memory notice after it means there is nothing to regenerate.
    for (std::size_t index = chatEntries.size(); index-- > *user + 1;)
    {
        const ChatEntry& entry = chatEntries[index];
        if (entry.kind != EntryKind::Message || entry.speaker.isEmpty() || entry.body.isEmpty())
            continue;
        if (entry.userMessage || entry.speaker != assistant || !EndsMessage(index))
            return std::nullopt;
        return index;
    }
    return std::nullopt;
}

void ReviaWindow::BeginEditLastTurn()
{
    const std::optional<std::size_t> user = LastUserEntry();
    if (!user)
        return;
    editingLastTurn = true;
    messageInput->setPlainText(chatEntries[*user].body);
    messageInput->moveCursor(QTextCursor::End);
    messageInput->setFocus();
    // A mode, not news: it stays above the message box until the edit is sent or cancelled.
    statusBar()->showMessage(
        QStringLiteral("Editing your last message. Send replaces it and ") + DisplayName() + QStringLiteral(" answers again; Esc cancels."), 15000);
    ShowComposerContext(QStringLiteral("Editing your last message"), QStringLiteral("Cancel (Esc)"), [this]() { CancelEditLastTurn(); });
    composerContextKind = ComposerContextKind::Editing;
}

void ReviaWindow::CancelEditLastTurn()
{
    editingLastTurn = false;
    messageInput->clear();
    if (composer)
        composer->ClearContext();
    composerContextKind = ComposerContextKind::None;
    ShowStatus(QStringLiteral("Edit cancelled. Your last message is unchanged."), 5000);
}

void ReviaWindow::RetryLastExchange(const QString& replacement)
{
    const bool editing = !replacement.isEmpty();
    const std::optional<std::size_t> user = LastUserEntry();
    if (switchingCompanion || shuttingDown.load() || !user || (!editing && !RegenerableReply()))
        return;
    if (!ClaimOperationWorker())
    {
        if (editing)
        {
            messageInput->setPlainText(replacement);
            editingLastTurn = true;
        }
        return;
    }
    CloseArchivedChat();
    lastTurnError.reset();
    userStopRequested = false;
    // What is replaced is kept, so a refusal can put the transcript back exactly. The
    // position is absolute because the transcript may be trimmed while this runs.
    const std::size_t keepFrom = *user + (editing ? 0 : 1);
    auto replaced = std::make_shared<std::vector<ChatEntry>>(
        chatEntries.begin() + static_cast<std::ptrdiff_t>(keepFrom), chatEntries.end());
    const std::size_t absoluteKeepFrom = keepFrom + chatEntriesDropped;
    chatEntries.resize(keepFrom);
    streamedReplyTurnId = 0;
    streamedReplyEntries.clear();
    chatTurnInFlight = true;
    if (editing)
        AppendChat("You", replacement, true);
    else
        RenderChat();
    stopButton->setEnabled(true);
    UpdateSendState();

    operationWorker = std::jthread(
        [this, epoch = sessionUiEpoch.load(), text = replacement.toStdString(), editing, replaced, absoluteKeepFrom]()
        {
            const RunningFlag running(operationRunning);
            bool retracted = false;
            const auto result = RunOrFail<revia::runtime::SessionResult>(
                [this, &text, editing, &retracted]()
                { return editing ? Session().ReplaceLastUserTurn(text, retracted) : Session().RegenerateLast(retracted); },
                [](const std::string& why)
                {
                    revia::runtime::SessionResult failed;
                    failed.succeeded = false;
                    failed.text = "That reply could not be replaced: " + why;
                    failed.reason = failed.text;
                    return failed;
                });
            QMetaObject::invokeMethod(
                this,
                [this, epoch, result, retracted, editing, replaced, absoluteKeepFrom, text]()
                {
                    if (epoch != sessionUiEpoch.load() || shuttingDown.load())
                        return;
                    chatTurnInFlight = false;
                    if (retracted)
                    {
                        PresentSessionResult(result);
                    }
                    else
                    {
                        // The session changed nothing, so neither does the transcript.
                        if (absoluteKeepFrom >= chatEntriesDropped && absoluteKeepFrom - chatEntriesDropped <= chatEntries.size())
                        {
                            const auto at = chatEntries.begin() + static_cast<std::ptrdiff_t>(absoluteKeepFrom - chatEntriesDropped);
                            const bool newLine = editing && at != chatEntries.end() && at->userMessage &&
                                                 at->body == QString::fromStdString(text);
                            const auto insertAt = newLine ? chatEntries.erase(at) : at;
                            chatEntries.insert(insertAt, replaced->begin(), replaced->end());
                            streamedReplyTurnId = 0;
                            streamedReplyEntries.clear();
                            TrimTranscript();
                        }
                        const QString why = QString::fromStdString(result.text.empty() ? result.reason : result.text);
                        ShowStatus(why, 12000);
                        AppendActivity((editing ? QStringLiteral("Edit not applied: ") : QStringLiteral("Regenerate not applied: ")) + why);
                    }
                    RenderChat();
                    RefreshStateBadge();
                    RefreshChatSidebar();
                    RefreshChatTitle();
                    messageInput->setFocus();
                },
                Qt::QueuedConnection);
        });
}

void ReviaWindow::RecordTurnError(const revia::runtime::SessionResult& result)
{
    // Drawn after the entry it followed; nothing is added to the transcript itself, so its
    // entry count is what it would have been without the failure.
    lastTurnError = TurnError{QString::fromStdString(result.reason), chatEntries.size(), 0, userStopRequested};
}

void ReviaWindow::RetryFailedTurn()
{
    const std::optional<std::size_t> user = LastUserEntry();
    if (!user || switchingCompanion || shuttingDown.load())
        return;
    // A reply that came back incomplete is regenerated; a turn that got no reply at all is sent
    // again with the same words, through the edit path, so nothing is removed.
    if (RegenerableReply())
        RetryLastExchange(QString());
    else
        RetryLastExchange(chatEntries[*user].body);
}

void ReviaWindow::CopyMessageAt(const QPoint& position)
{
    QMenu* menu = chatHistory->createStandardContextMenu(position);
    // The entry under the pointer: the nearest anchor named m<N> at or before it.
    std::optional<std::size_t> entryIndex;
    const QTextCursor cursor = chatHistory->cursorForPosition(position);
    for (QTextBlock block = cursor.block(); block.isValid() && !entryIndex; block = block.previous())
    {
        for (QTextBlock::iterator fragment = block.begin(); !fragment.atEnd(); ++fragment)
        {
            const QTextFragment piece = fragment.fragment();
            if (!piece.isValid() || (block == cursor.block() && piece.position() > cursor.position()))
                continue;
            for (const QString& anchor : piece.charFormat().anchorNames())
            {
                bool parsed = false;
                const std::size_t index = anchor.startsWith(QLatin1Char('m')) ? anchor.mid(1).toULongLong(&parsed) : 0;
                if (parsed && index < chatEntries.size())
                    entryIndex = index;
            }
        }
    }
    if (entryIndex && viewingArchivedChat.empty())
    {
        menu->addSeparator();
        QAction* copy = menu->addAction(QStringLiteral("Copy message"));
        const std::size_t first = *entryIndex;
        connect(copy, &QAction::triggered, this,
            [this, first]()
            {
                if (first >= chatEntries.size())
                    return;
                const ChatEntry& entry = chatEntries[first];
                QString text = entry.body;
                // A reply streamed in several entries is copied whole.
                if (entry.kind == EntryKind::Message && !entry.userMessage)
                {
                    std::size_t last = first;
                    while (last + 1 < chatEntries.size() && !EndsMessage(last))
                        ++last;
                    if (EndsMessage(last))
                        text = MessageText(last);
                }
                QApplication::clipboard()->setText(text);
                ShowStatus(QStringLiteral("Copied to the clipboard."), 3000);
            });
    }
    menu->exec(chatHistory->viewport()->mapToGlobal(position));
    delete menu;
}

void ReviaWindow::BuildChatSidebar()
{
    ChatSessionControls controls;
    controls.list = [this](const std::string& query, std::string& error) { return Session().BrowseConversations(query, 200, error); };
    controls.current = [this]() { return Session().CurrentConversationId(); };
    controls.startNew = [this](std::string& error) { return StartNewChat(error); };
    controls.rename = [this](const std::string& id, const std::string& title, std::string& error)
    {
        const bool renamed = Session().RenameConversation(id, title, error);
        if (renamed)
            QTimer::singleShot(0, this, [this]() { RefreshChatTitle(); });
        return renamed;
    };
    controls.remove = [this](const std::string& id, std::string& error) { return DeleteChat(id, error); };
    controls.exportText = [this](const std::string& id, const bool json, std::string& text, std::string& error)
    { return Session().ExportConversation(id, json, text, error); };
    controls.open = [this](const std::string& id) { OpenArchivedChat(id); };

    // The list is the sidebar's body (BuildShell places it); the chat page keeps the transcript.
    chatSessionsPanel = new ChatSessionsPanel(std::move(controls), this);
    chatSidebarToggle = chatSessionsPanel->ListToggle();
    chatSessionsPanel->SetListVisible(QSettings().value("chat/sidebarVisible", true).toBool());
    connect(chatSidebarToggle, &QPushButton::toggled, this,
        [this](const bool shown)
        {
            // The same setting as Settings › Appearance › Show recent chats in the sidebar.
            QSettings().setValue("chat/sidebarVisible", shown);
            if (settingsDialog)
                settingsDialog->SetValue("chat/sidebarVisible", shown);
        });

    // In the transcript's 760px measure, like the composer (ApplyComposerMeasure).
    archivedBannerHost = new QWidget(chatPage);
    auto* bannerRow = new QHBoxLayout(archivedBannerHost);
    bannerRow->setContentsMargins(16, 0, 16, 0);
    archivedChatBanner = new QWidget(archivedBannerHost);
    archivedChatBanner->setObjectName("archivedChatBanner");
    archivedChatBanner->setAttribute(Qt::WA_StyledBackground, true);
    bannerRow->addWidget(archivedChatBanner);
    auto* bannerLayout = new QHBoxLayout(archivedChatBanner);
    bannerLayout->setContentsMargins(12, 8, 8, 8);
    archivedChatLabel = new QLabel(archivedChatBanner);
    archivedChatLabel->setObjectName("archivedChatLabel");
    archivedChatLabel->setWordWrap(true);
    bannerLayout->addWidget(archivedChatLabel, 1);
    continueChatButton = new QPushButton(QStringLiteral("Continue this chat"), archivedChatBanner);
    continueChatButton->setObjectName("continueChatButton");
    continueChatButton->setToolTip(QStringLiteral("Make this the current chat. Only your own turns from it go back into the conversation."));
    bannerLayout->addWidget(continueChatButton);
    auto* back = new QPushButton(QStringLiteral("Back to current chat"), archivedChatBanner);
    back->setObjectName("backToChatButton");
    bannerLayout->addWidget(back);
    archivedChatBanner->hide();
    ui->chatLayout->insertWidget(ui->chatLayout->indexOf(chatHistory), archivedBannerHost);
    connect(continueChatButton, &QPushButton::clicked, this, [this]() { ContinueArchivedChat(); });
    connect(back, &QPushButton::clicked, this, [this]() { CloseArchivedChat(); });
    chatSessionsPanel->Refresh();
}

void ReviaWindow::RefreshChatSidebar()
{
    // Refreshed while folded or hidden in the rail too: the chat header's title comes from it.
    if (chatSessionsPanel)
        chatSessionsPanel->Refresh();
    RefreshChatTitle();
}

void ReviaWindow::ClearTranscript()
{
    chatEntries.clear();
    streamedReplyTurnId = 0;
    streamedReplyEntries.clear();
    pendingUtterances.clear();
    pendingSpeechTimer->stop();
    RenderChat();
}

bool ReviaWindow::StartNewChat(std::string& outError)
{
    if (operationRunning.load())
    {
        outError = DisplayName().toStdString() + " is still working on the previous request. Start a new chat once the reply is in.";
        return false;
    }
    if (!Session().StartNewConversation(outError))
        return false;
    CloseArchivedChat();
    lastTurnError.reset();
    ClearTranscript();
    RefreshChatTitle();
    messageInput->setFocus();
    return true;
}

bool ReviaWindow::DeleteChat(const std::string& id, std::string& outError)
{
    const bool current = id == Session().CurrentConversationId();
    if (current && operationRunning.load())
    {
        outError = DisplayName().toStdString() + " is still working on the previous request. Delete this chat once the reply is in.";
        return false;
    }
    if (!Session().DeleteConversation(id, outError))
        return false;
    if (viewingArchivedChat == id)
        CloseArchivedChat();
    if (current)
        ClearTranscript();
    return true;
}

void ReviaWindow::OpenArchivedChat(const std::string& id)
{
    if (id.empty() || id == Session().CurrentConversationId())
    {
        CloseArchivedChat();
        return;
    }
    std::string error;
    const std::vector<revia::memory::ArchivedTurn> turns = Session().ConversationTranscript(id, error);
    if (!error.empty())
    {
        chatSessionsPanel->SetStatus(QString::fromStdString(error));
        return;
    }
    viewingArchivedChat = id;
    archivedChatLabel->setText(QStringLiteral("Reading a saved chat. It is not part of the current conversation."));
    continueChatButton->setEnabled(!turns.empty());
    archivedChatBanner->show();
    RenderArchivedChat(turns);
    chatHistory->verticalScrollBar()->setValue(0);
}

void ReviaWindow::RenderArchivedChat(const std::vector<revia::memory::ArchivedTurn>& turns)
{
    // Drawn like the live transcript. Reply text from the archive is as untrusted as it was live.
    const QString assistant = DisplayName();
    QString html;
    bool named = false;
    for (const revia::memory::ArchivedTurn& turn : turns)
    {
        const QString body = QString::fromStdString(turn.content);
        if (turn.role == "user")
        {
            html += UserBubbleHtml(body);
            continue;
        }
        if (!named)
            html += QStringLiteral("<p class=\"speaker\"><span style=\"color:%1;\">&#9679;</span>&nbsp;%2</p>")
                        .arg(revia::desktop::theme::Current().accent.name(), assistant.toHtmlEscaped());
        named = true;
        html += QStringLiteral("<div class=\"assistant\">") + revia::desktop::RenderUntrustedMarkdown(body) + QStringLiteral("</div>");
    }
    if (turns.empty())
        html = QStringLiteral("<p class=\"archivedNote\">Nothing from this chat was kept in the archive.</p>");
    chatHistory->document()->setDefaultStyleSheet(TranscriptCss());
    chatHistory->setHtml(html);
    ApplyChatMeasure();
}

void ReviaWindow::CloseArchivedChat()
{
    if (viewingArchivedChat.empty())
        return;
    viewingArchivedChat.clear();
    archivedChatBanner->hide();
    RenderChat();
}

void ReviaWindow::ContinueArchivedChat()
{
    const std::string id = viewingArchivedChat;
    if (id.empty())
        return;
    if (operationRunning.load())
    {
        archivedChatLabel->setText(DisplayName() + QStringLiteral(" is still working on the previous request. Continue once the reply is in."));
        return;
    }
    std::string error;
    if (!Session().ContinueConversation(id, error))
    {
        archivedChatLabel->setText(QString::fromStdString(error));
        return;
    }
    // The whole saved chat is shown; Revia's working context holds only the current
    // speaker's own turns from it.
    const std::vector<revia::memory::ArchivedTurn> turns = Session().ConversationTranscript(id, error);
    viewingArchivedChat.clear();
    archivedChatBanner->hide();
    chatEntries.clear();
    lastTurnError.reset();
    streamedReplyTurnId = 0;
    streamedReplyEntries.clear();
    const QString assistant = QString::fromStdString(Session().DisplayName());
    for (const revia::memory::ArchivedTurn& turn : turns)
    {
        ChatEntry entry;
        entry.userMessage = turn.role == "user";
        entry.speaker = entry.userMessage ? QStringLiteral("You") : assistant;
        entry.body = QString::fromStdString(turn.content);
        entry.markdown = !entry.userMessage;
        chatEntries.push_back(std::move(entry));
    }
    TrimTranscript();
    RenderChat();
    RefreshChatSidebar();
    messageInput->setFocus();
}

void ReviaWindow::AppendComponentActivity(const revia::runtime::RuntimeEvent& event, const QString& message)
{
    const bool error = event.phase == "Error" || event.phase == "Failed";
    const bool warning = event.phase == "Unavailable" || event.phase == "Partial" || event.phase == "Warning" || event.phase == "Blocked";
    const QString key = QString::fromStdString(event.component + ":" + event.phase);
    if (error || warning)
    {
        // Keep the first issue and periodic reminders. Elapsed timings change on every
        // retry, so deduplicate the stable status message rather than the rendered line.
        const QString cause = QString::fromStdString(event.message);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const auto previous = lastComponentIssues.find(key);
        if (previous != lastComponentIssues.end() && previous->second.first == cause && now - previous->second.second < 60000)
            return;
        lastComponentIssues[key] = {cause, now};
    }
    else if (event.phase == "Aware" || event.phase == "Kept private" || event.phase == "Spoke" || event.phase == "Ready")
    {
        for (const char* phase : {"Error", "Failed", "Unavailable", "Partial"})
            lastComponentIssues.erase(QString::fromStdString(event.component + ":" + phase));
    }
    // Structured background statuses have known severity. Words inside a successful
    // observation ("error", "failed build") must not create a runtime error badge.
    const bool background = event.component == "Vision" || event.component == "Curiosity" || event.component == "Load";
    AppendActivity(message, error        ? ActivitySeverity::Error
                            : warning    ? ActivitySeverity::Warning
                            : background ? ActivitySeverity::Information
                                         : ActivitySeverity::Automatic);
}

void ReviaWindow::AppendActivity(const QString& inputMessage, ActivitySeverity severity)
{
    QString message = inputMessage.trimmed();
    if (message.isEmpty())
    {
        return;
    }
    if (!message.startsWith(QLatin1Char('[')))
    {
        message =
            QStringLiteral("[") + QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")) + QStringLiteral("] ") + message;
    }
    if (severity == ActivitySeverity::Automatic)
    {
        const QString lowered = message.toLower();
        if (lowered.contains(QStringLiteral("] [error]")) || lowered.contains(QStringLiteral(" error:")) ||
            lowered.contains(QStringLiteral(" failed")) || lowered.contains(QStringLiteral(" failure")) ||
            lowered.contains(QStringLiteral(" could not")))
        {
            severity = ActivitySeverity::Error;
        }
        else if (lowered.contains(QStringLiteral("] [warning]")) || lowered.contains(QStringLiteral(" warning")) ||
                 lowered.contains(QStringLiteral(" degraded")) || lowered.contains(QStringLiteral(" fallback")) ||
                 lowered.contains(QStringLiteral(" unavailable")) || lowered.contains(QStringLiteral(" blocked")) ||
                 lowered.contains(QStringLiteral(" refused")) || lowered.contains(QStringLiteral(" missing")))
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
        activityEntries.erase(activityEntries.begin(),
            activityEntries.begin() + static_cast<std::ptrdiff_t>(activityEntries.size() - MaximumVisibleLogEntries));
        // Recounted from what is still held, because the counters describe the retained
        // model and trimming changes that model. Incrementing on append but never
        // decrementing on trim is what let the summary report errors that had already
        // scrolled out of existence -- at 2000 entries a full recount is trivial, and it
        // cannot drift the way paired increments and decrements can.
        std::vector<int> retained;
        retained.reserve(activityEntries.size());
        for (const ActivityEntry& entry : activityEntries)
        {
            retained.push_back(entry.severity == ActivitySeverity::Warning ? 1 : entry.severity == ActivitySeverity::Error ? 2 : 0);
        }
        const revia::runtime::RetainedSeverityCounts counts = revia::runtime::CountRetainedSeverities(retained);
        activityWarningCount = counts.warnings;
        activityErrorCount = counts.errors;
    }
    const int filter = activityFilter == nullptr ? 0 : activityFilter->currentIndex();
    const bool visible =
        (filter == 0) || (filter == 1 && severity != ActivitySeverity::Information) || (filter == 2 && severity == ActivitySeverity::Error);
    if (trimmedOldEntries)
    {
        RenderActivity();
    }
    else if (visible && activityFeed != nullptr)
    {
        const QString color = ActivityColour(severity);
        activityFeed->append(
            QStringLiteral("<span style=\"color:%1; white-space:pre-wrap;\">%2</span>").arg(color, message.toHtmlEscaped()));
        if (activityAutoScroll != nullptr && activityAutoScroll->isChecked())
        {
            activityFeed->verticalScrollBar()->setValue(activityFeed->verticalScrollBar()->maximum());
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
        if ((filter == 1 && entry.severity == ActivitySeverity::Information) || (filter == 2 && entry.severity != ActivitySeverity::Error))
        {
            continue;
        }
        const QString color = ActivityColour(entry.severity);
        html += QStringLiteral("<div style=\"color:%1; margin:0 0 4px 0; white-space:pre-wrap;\">%2</div>")
                    .arg(color, entry.message.toHtmlEscaped());
    }
    activityFeed->setHtml(html);
    if (activityAutoScroll != nullptr && activityAutoScroll->isChecked())
    {
        activityFeed->verticalScrollBar()->setValue(activityFeed->verticalScrollBar()->maximum());
    }
    else
    {
        activityFeed->verticalScrollBar()->setValue(previousScroll);
    }
}

QString ReviaWindow::ActivityColour(const ActivitySeverity severity) const
{
    const revia::desktop::theme::Tokens& tokens = revia::desktop::theme::Current();
    return (severity == ActivitySeverity::Error ? tokens.danger : severity == ActivitySeverity::Warning ? tokens.warning : tokens.text).name();
}

void ReviaWindow::UpdateActivitySummary()
{
    if (activityIssueSummary == nullptr)
    {
        return;
    }
    const bool hasErrors = activityErrorCount > 0;
    const bool hasWarnings = activityWarningCount > 0;
    activityIssueSummary->setText(!hasErrors && !hasWarnings ? QStringLiteral("No warnings or errors")
                                                             : QStringLiteral("%1 error%2  •  %3 warning%4")
                                                                   .arg(activityErrorCount)
                                                                   .arg(activityErrorCount == 1 ? QString() : QStringLiteral("s"))
                                                                   .arg(activityWarningCount)
                                                                   .arg(activityWarningCount == 1 ? QString() : QStringLiteral("s")));
    // Labelled for what it counts. "Retained" is not a detail: the log holds the most
    // recent 2000 entries, so this is a count of what is still there rather than a
    // lifetime total, and conflating the two is what made the old number wrong.
    activityIssueSummary->setToolTip(
        QStringLiteral("Warnings and errors in the retained activity log (most recent %1 entries).").arg(2000));
    activityIssueSummary->setProperty(
        "level", hasErrors ? QStringLiteral("error") : (hasWarnings ? QStringLiteral("warning") : QStringLiteral("ok")));
    Repolish(activityIssueSummary);

    // The count rides on the Activity window's title and menu entry, and a dot on the avatar.
    const int issueCount = activityErrorCount + activityWarningCount;
    if (panelHost)
        panelHost->SetTitleSuffix(revia::desktop::PanelId::Activity, issueCount == 0 ? QString() : QStringLiteral(" (%1)").arg(issueCount));
    if (navigationSidebar)
        navigationSidebar->SetAttention(issueCount > 0);
}

void ReviaWindow::ApplyUserPreferences()
{
    // The switches are the Settings dialog's and read their own stores; the two number rows
    // the window made are read here.
    if (settingsDialog)
        settingsDialog->Refresh();
    RefreshPreferenceSpins();
    RefreshCloudUseFor();
    RefreshPresenceUi();
}

void ReviaWindow::RefreshPresenceUi()
{
    if (presencePhaseValue == nullptr)
        return;
    const revia::presence::PresenceSnapshot snapshot = Session().Presence();
    presencePhaseValue->setText(QString::fromStdString(snapshot.phase));
    presenceAffectValue->setText(QString::fromStdString(revia::runtime::ToString(snapshot.affect)) +
                                 QStringLiteral(" %1%").arg(qRound(snapshot.affectIntensity * 100.0F)));
    presenceAttentionValue->setText(QString::fromStdString(snapshot.attention));
    presenceAttentionValue->setToolTip(QString::fromStdString(snapshot.attention));
    presenceMomentumBar->setValue(qRound(snapshot.conversationMomentum * 100.0F));
}

void ReviaWindow::ShowPreferenceResult(const revia::core::PreferenceResult& result)
{
    const QString message = QString::fromStdString(result.message);
    ShowStatus(message, result.succeeded ? 5000 : 8000);
    AppendActivity(QStringLiteral("Setting: ") + message, result.succeeded ? ActivitySeverity::Information : ActivitySeverity::Warning);
}

std::function<bool()> ReviaWindow::CaptureQuestionAdmission()
{
    const auto origin = Session().Stamp();
    const auto audience = Session().Audience();
    const auto epoch = sessionUiEpoch.load();
    return [this, origin, audience, epoch]()
    {
        if (epoch != sessionUiEpoch.load() || shuttingDown.load())
            return false;
        const auto currentOrigin = Session().Stamp();
        const auto currentAudience = Session().Audience();
        return origin.SameSession(currentOrigin) && origin.policyVersion == currentOrigin.policyVersion &&
               audience.kind == revia::identity::AudienceKind::Private && audience.revision != 0 && currentAudience.kind == audience.kind &&
               currentAudience.revision == audience.revision;
    };
}

bool ReviaWindow::RunAdmittedQuestion(QMessageBox& question, const std::function<bool()>& admitted)
{
    if (!admitted())
        return false;
    QTimer admissionTimer;
    connect(&admissionTimer, &QTimer::timeout, &question,
        [&question, admitted]()
        {
            if (!admitted())
                question.reject();
        });
    admissionTimer.start(25);
    if (!admitted())
        return false;
    openQuestion = &question;
    question.exec();
    return admitted();
}

bool ReviaWindow::ApproveDesktopEffect(const revia::policy::ApprovalPrompt& prompt)
{
    const auto admitted = CaptureQuestionAdmission();
    // A copy: the question can outlive the caller's wait during shutdown.
    std::function<bool()> ask = [this, prompt, admitted]()
    {
        if (!admitted())
            return false;
        // Same reason ConfirmAction does this: a question owned by a minimized window is
        // invisible, and the worker would wait on an answer nobody can see.
        if (isMinimized())
        {
            showNormal();
            raise();
            activateWindow();
        }
        const QString control =
            prompt.controlName.empty() ? QStringLiteral("An unnamed control") : "\"" + QString::fromStdString(prompt.controlName) + "\"";
        QString description = control + " is about to be activated.\n\n" + QString::fromStdString(prompt.reason);
        if (!prompt.application.empty())
        {
            description += "\n\nApplication: " + QString::fromStdString(prompt.application);
        }
        if (!prompt.windowTitle.empty())
        {
            description += "\nWindow: " + QString::fromStdString(prompt.windowTitle);
        }
        // Says what is being approved and what is not. The prompt never carries the text
        // being sent, so this cannot become a preview of content it does not have.
        description += "\n\nThis approves this one activation, now. It does not raise "
                       "any permission and does not apply to the next one.";
        QMessageBox question(QMessageBox::Question, "Approve this action", description, QMessageBox::Yes | QMessageBox::No, this);
        question.setDefaultButton(QMessageBox::No);
        return RunAdmittedQuestion(question, admitted) && question.result() == QMessageBox::Yes;
    };
    return questions.Ask<bool>(PostToWindow(), QThread::currentThread() == thread(), std::move(ask), false);
}

revia::actions::ConfirmationChoice ReviaWindow::ConfirmAction(
    const revia::actions::ActionRequest& request, const revia::actions::PolicyDecision& decision)
{
    using revia::actions::ConfirmationChoice;
    const auto admitted = CaptureQuestionAdmission();
    // Copies: the question can outlive the caller's wait during shutdown.
    std::function<ConfirmationChoice()> showConfirmation = [this, request, decision, admitted]()
    {
        if (!admitted())
            return ConfirmationChoice::Decline;
        // A screen action hides Revia before capture so it cannot obscure the target.
        // Restore the shell before asking; a confirmation owned by a minimized window is
        // effectively invisible and would leave the worker waiting forever.
        if (isMinimized())
        {
            showNormal();
            raise();
            activateWindow();
        }
        QString description =
            "Action: " + QString::fromStdString(revia::actions::ToString(request.type)) +
            (request.application.empty() ? "\nSource: " + QString::fromStdString(revia::actions::PathToUtf8(request.source))
                                         : "\nApplication: " + QString::fromStdString(request.application) +
                                               "\nWindow: " + QString::fromStdString(request.windowTitle) +
                                               "\nControl: " + QString::fromStdString(request.control));
        if (!request.destination.empty())
        {
            description += "\nDestination: " + QString::fromStdString(revia::actions::PathToUtf8(request.destination));
        }
        // A guest action names no host source; what is approved is its program, command,
        // file or folder, so that is what the prompt shows.
        if (revia::actions::IsGuestAction(request.type))
            description += "\n\n" + QString::fromStdString(revia::actions::DescribeGuestPayload(request));
        description += "\n\nPolicy: " + QString::fromStdString(decision.reason);
        if (request.resolution.IsUiaElementTarget())
        {
            description += "\n\nVision target: " + QString::fromStdString(request.resolution.modelTarget) +
                           "\nResolved UIA element: " + QString::fromStdString(request.resolution.resolvedName) +
                           "\nMatch confidence: " + QString::number(request.resolution.matchConfidence * 100.0, 'f', 1) + "%" +
                           "\nThe exact UIA runtime id must still match when you approve.";
        }
        // Three answers, because driving a browser is a focus, a chord, a type and an
        // enter, and asking four times for one sentence is how a safety prompt becomes
        // something people dismiss without reading. The middle button says exactly what
        // it covers so that consent stays informed: this task, this risk level, and
        // nothing that escalates.
        QMessageBox prompt(this);
        prompt.setWindowTitle("Confirm Revia action");
        prompt.setText(description);
        const bool wholeTask = request.requestedBy == "goal";
        QPushButton* once = wholeTask ? nullptr : prompt.addButton("Allow once", QMessageBox::AcceptRole);
        QPushButton* everything = prompt.addButton("Allow for this whole task", QMessageBox::AcceptRole);
        QPushButton* refuse = prompt.addButton("No", QMessageBox::RejectRole);
        everything->setToolTip(wholeTask ? "Approve the task described above. Work outside its scope stops; approval ends with the task."
                                         : "Stops asking for the rest of this one task, for this same kind of action in "
                                           "this same application, no riskier than this. A different kind of action asks "
                                           "again, and so does anything that sends, buys, deletes or opens a file with an "
                                           "application. It is forgotten when the task ends and changes no permission.");
        prompt.setDefaultButton(refuse);
        prompt.setEscapeButton(refuse);
        if (!RunAdmittedQuestion(prompt, admitted))
            return ConfirmationChoice::Decline;
        if (prompt.clickedButton() == everything)
        {
            return ConfirmationChoice::AllowForThisTask;
        }
        if (once != nullptr && prompt.clickedButton() == once)
        {
            return ConfirmationChoice::Allow;
        }
        return ConfirmationChoice::Decline;
    };
    return questions.Ask<ConfirmationChoice>(
        PostToWindow(), QThread::currentThread() == thread(), std::move(showConfirmation), ConfirmationChoice::Decline);
}

revia::core::QuestionRelay::Post ReviaWindow::PostToWindow()
{
    return [this](std::function<void()> work)
    {
        // Queued, never blocking: Qt drops it if the window is destroyed first.
        QMetaObject::invokeMethod(this, std::move(work), Qt::QueuedConnection);
    };
}

void ReviaWindow::AbandonQuestions()
{
    questions.Abandon();
    // An approval already on screen would hold shutdown until someone answered it.
    if (openQuestion)
        openQuestion->reject();
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


void ReviaWindow::BuildAttachmentControls()
{
    attachmentTray = new AttachmentTray([this](const std::string& id) { RemoveAttachmentChip(id); }, ui->chatPage);
    ui->chatLayout->insertWidget(ui->chatLayout->indexOf(messageInput), attachmentTray);
    attachButton = new QPushButton(QStringLiteral("Attach"), ui->chatPage);
    attachButton->setObjectName(QStringLiteral("attachButton"));
    attachButton->setToolTip(QStringLiteral("Attach pictures, PDF, Word (.docx) or text files. You can also drag files onto the chat or "
                                            "paste a picture. Files stay on this computer."));
    documentsButton = new QPushButton(QStringLiteral("Documents"), ui->chatPage);
    documentsButton->setObjectName(QStringLiteral("documentsButton"));
    documentsButton->setToolTip(QStringLiteral("See, open or forget the files you have attached."));
    ui->chatButtonLayout->insertWidget(2, attachButton);
    ui->chatButtonLayout->insertWidget(3, documentsButton);
    connect(attachButton, &QPushButton::clicked, this, [this]() { ChooseAttachments(); });
    connect(documentsButton, &QPushButton::clicked, this, [this]() { ShowDocuments(); });
    // Drops land on the viewports: QPlainTextEdit and QTextBrowser deliver drag events there.
    messageInput->viewport()->installEventFilter(this);
    chatHistory->viewport()->installEventFilter(this);
    messageInput->viewport()->setAcceptDrops(true);
    chatHistory->viewport()->setAcceptDrops(true);
}

void ReviaWindow::ChooseAttachments()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("Attach files"), QString(),
        QStringLiteral("Supported files (*.png *.jpg *.jpeg *.gif *.webp *.bmp *.pdf *.docx *.txt *.md *.csv *.tsv *.json *.log *.xml "
                       "*.yaml *.yml *.toml *.ini *.c *.cc *.cpp *.h *.hpp *.cs *.py *.js *.ts *.java *.rs *.go *.html *.css *.sql "
                       "*.ps1 *.sh);;All files (*)"));
    AttachPaths(paths);
}

void ReviaWindow::AttachPaths(const QStringList& paths)
{
    if (paths.isEmpty() || shuttingDown.load() || switchingCompanion)
        return;
    const std::size_t limit = revia::attachments::AttachmentLimits{}.maxAttachmentsPerTurn;
    if (attachmentTray->Chips().size() + static_cast<std::size_t>(paths.size()) > limit)
    {
        attachmentTray->SetStatus(QStringLiteral("A message can carry at most %1 files.").arg(limit));
        return;
    }
    if (attachmentRunning.exchange(true))
    {
        attachmentTray->SetStatus(QStringLiteral("Still reading the previous attachment. Try again in a moment."));
        return;
    }
    // The previous worker has finished its body, so this join does not wait on work.
    if (attachmentWorker.joinable())
        attachmentWorker.join();
    attachmentTray->SetStatus(paths.size() == 1 ? QStringLiteral("Reading %1...").arg(QFileInfo(paths.front()).fileName())
                                                : QStringLiteral("Reading %1 files...").arg(paths.size()));
    attachmentWorker = std::jthread(
        [this, epoch = sessionUiEpoch.load(), paths]()
        {
            const RunningFlag running(attachmentRunning);
            std::vector<AttachmentOutcome> outcomes;
            for (const QString& path : paths)
            {
                try
                {
                    const std::filesystem::path native(path.toStdU16String());
                    // Pictures are decoded and shrunk by Qt here; everything else is read by
                    // the session's own intake. Neither opens the file in another program.
                    std::optional<PreparedImage> prepared;
                    if (LooksLikeImageFile(path))
                        prepared = PrepareImageFileForAttachment(path, static_cast<int>(revia::attachments::DesktopImageSide));
                    if (prepared && !prepared->png.isEmpty())
                        outcomes.push_back(OutcomeOf(
                            Session().AttachImageData(QFileInfo(path).fileName().toStdString(), prepared->png.toStdString()),
                            prepared->thumbnail));
                    else
                        outcomes.push_back(OutcomeOf(Session().AttachFile(native), QImage()));
                }
                catch (const std::exception& error)
                {
                    AttachmentOutcome failed;
                    failed.message = QStringLiteral("\"%1\" could not be attached: %2").arg(QFileInfo(path).fileName(), error.what());
                    outcomes.push_back(failed);
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, epoch, outcomes]()
                {
                    if (epoch != sessionUiEpoch.load() || shuttingDown.load())
                        return;
                    QStringList refused;
                    for (const AttachmentOutcome& outcome : outcomes)
                    {
                        if (outcome.accepted)
                            attachmentTray->Add(outcome.chip);
                        else
                            refused << outcome.message;
                    }
                    attachmentTray->SetStatus(refused.join(QStringLiteral(" ")));
                    messageInput->setFocus();
                },
                Qt::QueuedConnection);
        });
}

void ReviaWindow::AttachPastedImage(const QImage& image)
{
    if (attachmentTray->Chips().size() >= revia::attachments::AttachmentLimits{}.maxAttachmentsPerTurn)
    {
        attachmentTray->SetStatus(QStringLiteral("A message can carry at most %1 files.")
                                      .arg(revia::attachments::AttachmentLimits{}.maxAttachmentsPerTurn));
        return;
    }
    const PreparedImage prepared = PrepareImageForAttachment(image, static_cast<int>(revia::attachments::DesktopImageSide));
    if (prepared.png.isEmpty())
    {
        attachmentTray->SetStatus(prepared.error);
        return;
    }
    const QString name = QStringLiteral("pasted-image-%1.png").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const AttachmentOutcome outcome = OutcomeOf(Session().AttachImageData(name.toStdString(), prepared.png.toStdString()), prepared.thumbnail);
    if (outcome.accepted)
        attachmentTray->Add(outcome.chip);
    attachmentTray->SetStatus(outcome.accepted ? QString() : outcome.message);
}

void ReviaWindow::RemoveAttachmentChip(const std::string& id)
{
    (void)Session().RemoveAttachment(id);
    attachmentTray->Remove(id);
    attachmentTray->SetStatus(QString());
}

bool ReviaWindow::HandleAttachmentDrag(QEvent* event)
{
    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove)
    {
        auto* drag = static_cast<QDragMoveEvent*>(event);
        const QMimeData* data = drag->mimeData();
        if (!LocalFiles(data).isEmpty() || (data && data->hasImage()))
        {
            drag->acceptProposedAction();
            return true;
        }
        return false;
    }
    if (event->type() == QEvent::Drop)
    {
        auto* drop = static_cast<QDropEvent*>(event);
        const QMimeData* data = drop->mimeData();
        const QStringList files = LocalFiles(data);
        if (!files.isEmpty())
        {
            drop->acceptProposedAction();
            AttachPaths(files);
            return true;
        }
        if (data && data->hasImage())
        {
            drop->acceptProposedAction();
            AttachPastedImage(qvariant_cast<QImage>(data->imageData()));
            return true;
        }
    }
    return false;
}

void ReviaWindow::ShowDocuments()
{
    if (!documentsDialog)
    {
        DocumentControls controls;
        controls.list = [this]() { return Session().Documents(); };
        controls.preview = [this](const std::string& id) { return Session().DocumentPreview(id); };
        controls.forget = [this](const std::string& id, std::string& error) { return Session().ForgetDocument(id, error); };
        documentsDialog = new DocumentsDialog(std::move(controls), this);
        documentsDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    documentsDialog->Refresh();
    documentsDialog->show();
    documentsDialog->raise();
    documentsDialog->activateWindow();
}
