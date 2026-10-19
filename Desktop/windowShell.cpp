// The shell around the existing pages (design spec 2 to 5): the panel windows, the Settings
// dialog and its session backend, the left sidebar, the chat column with its header, composer and
// overlays, the menus, the command palette and the shortcuts. ReviaWindow members; split from
// reviaWindow.cpp, which keeps the runtime wiring.

#include "reviaWindow.h"

#include "attachmentTray.h"
#include "capabilityPanel.h"
#include "chatSessionsPanel.h"
#include "commandPalette.h"
#include "composerCard.h"
#include "conversationStatusPanel.h"
#include "desktopObjectNames.h"
#include "panelWindow.h"
#include "profilePanel.h"
#include "serialWorker.h"
#include "settingsCatalog.h"
#include "settingsDialog.h"
#include "settingsSessionBackend.h"
#include "shellSidebar.h"
#include "shortcutSheet.h"
#include "softShadow.h"
#include "tabNavigation.h"
#include "theme.h"
#include "toastOverlay.h"
#include "voiceHealthPanel.h"
#include "ui_reviaWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStyle>
#include <QSpinBox>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTextBrowser>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QWidgetAction>

#include <algorithm>
#include <exception>

namespace
{
namespace desktop = revia::desktop;
namespace theme = revia::desktop::theme;
using desktop::PanelId;
using desktop::SettingsSection;

// The epoch a Settings write was queued under, for the callback it posts back: a write that
// finishes after a companion switch must not touch the new companion's rows.
std::uint64_t& WriteEpoch()
{
    thread_local std::uint64_t epoch = 0;
    return epoch;
}

QIcon ShellIcon(const char* name)
{
    return theme::Icon(name);
}

// Icon-only header and toolbar buttons: a name for screen readers, a tooltip that carries the
// shortcut, and no focus ring left behind by a click.
QToolButton* IconButton(QWidget* parent, const char* objectName, const char* icon, const QString& name, const QString& tip)
{
    auto* button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setAccessibleName(name);
    button->setToolTip(tip);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::TabFocus);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setFixedSize(32, 32);
    theme::BindIcon(button, icon);
    return button;
}

// The question Settings asks before a risky change, for the window's own controls in it: the
// question itself as the message, the consequence under it, and Cancel as the default.
void AskBeforeRiskyChoice(QWidget* parent, const QString& question, const QString& consequence, std::function<void()> accepted)
{
    auto* box = new QMessageBox(parent);
    box->setObjectName("settingConfirmDialog");
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(question);
    box->setTextFormat(Qt::PlainText);
    box->setText(question);
    box->setInformativeText(consequence + QStringLiteral(" You can change this back here at any time."));
    QPushButton* accept = box->addButton(QStringLiteral("Turn on"), QMessageBox::AcceptRole);
    // Added to the button box, the button is already polished: the property needs a repolish.
    accept->setProperty("destructive", true);
    accept->style()->unpolish(accept);
    accept->style()->polish(accept);
    QPushButton* cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(cancel);
    box->setEscapeButton(cancel);
    QObject::connect(box, &QMessageBox::finished, parent, [box, accept, accepted = std::move(accepted)](int)
    {
        if (box->clickedButton() == accept)
            accepted();
    });
    box->open();
}

QLabel* StatusChip(QWidget* parent, const char* objectName, const QString& text)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMinimumWidth(150);
    return label;
}
}

QString ReviaWindow::DisplayName() const
{
    const QString name = QString::fromStdString(Session().DisplayName());
    return name.isEmpty() ? QStringLiteral("Revia") : name;
}

bool ReviaWindow::DesktopControlActive() const
{
    return automationRunning && !Session().DesktopControlStopped();
}

void ReviaWindow::BuildPanels()
{
    panelHost = std::make_unique<desktop::PanelHost>(this);
    panelHost->SetIconProvider(&ShellIcon);
    panelHost->SetShowHook([](QWidget* window) { theme::ApplyNativeFrame(window); });
    panelHost->SetSettingsHook([this](const int section) { OpenSettings(desktop::SectionName(static_cast<SettingsSection>(section))); });
    panelHost->SetResizeHook(PanelId::Activity, [this](const QSize size) { ApplyActivityLayout(size.width()); });
    panelHost->SetDefaultFocus(messageInput);
    // Owned windows stay above the main window; un-owning them is an untested Windows experiment
    // (docs/DESKTOP_NAVIGATION.md), so the owned fallback ships.
    panelHost->SetUnowned(false);
    for (const desktop::PanelSpec& spec : desktop::PanelCatalog())
    {
        QWidget* page = findChild<QWidget*>(QString::fromLatin1(spec.pageObjectName));
        if (page == nullptr)
            continue;
        panelHost->Adopt(spec.id, page);
        desktop::PolishSectionTabs(page);
    }
}

void ReviaWindow::BuildSettingsDialog()
{
    using desktop::SettingsSection;
    settingsWorker = std::make_unique<desktop::SerialWorker>();

    desktop::SessionBackendDeps deps;
    deps.session = [this]() -> revia::runtime::ReviaSession& { return Session(); };
    deps.runInBackground = [this](std::function<void()> work)
    {
        const std::uint64_t epoch = sessionUiEpoch.load();
        settingsWorker->Post(
            [epoch, work = std::move(work)]()
            {
                WriteEpoch() = epoch;
                work();
            });
    };
    deps.postToUi = [this](std::function<void()> callback)
    {
        // On the worker, the epoch the write was queued under; on the UI thread (a write refused
        // before it started), the current one.
        const std::uint64_t epoch = QThread::currentThread() == thread() ? sessionUiEpoch.load() : WriteEpoch();
        QMetaObject::invokeMethod(
            this,
            [this, epoch, callback = std::move(callback)]()
            {
                if (epoch != sessionUiEpoch.load() || shuttingDown.load())
                    return;
                callback();
            },
            Qt::QueuedConnection);
    };
    deps.profileDraftDirty = [this]() { return profilePanel != nullptr && profilePanel->HasUnsavedChanges(); };
    deps.afterProfileWrite = [this]()
    {
        if (profilePanel)
            profilePanel->Refresh();
        RefreshAccount();
    };
    const desktop::SettingsBackend backend = desktop::MakeSessionSettingsBackend(std::move(deps));

    desktop::SettingsHooks hooks;
    hooks.valueChanged = [this](const std::string& key, const bool value) { ApplyShellSetting(key, value); };
    hooks.themePreference = []() { return static_cast<int>(theme::CurrentPreference()); };
    hooks.setThemePreference = [](const int preference)
    { theme::SetPreference(static_cast<theme::Preference>(std::clamp(preference, 0, 2))); };
    hooks.chatTextSize = [this]() { return chatTextSize; };
    hooks.setChatTextSize = [this](const int pixels)
    {
        QSettings().setValue("appearance/chatTextSize", pixels);
        ApplyChatTextSize(pixels);
    };
    hooks.openPanel = [this](const char* key)
    {
        if (const auto panel = desktop::PanelFromKey(QString::fromLatin1(key)))
            OpenPanel(*panel);
    };
    hooks.restartApp = [this]()
    {
        restartRequested = true;
        BeginShutdown(revia::core::ExitReason::UserCommand, "restart from Settings");
    };
    // Unsaved list edits in Permissions never outlive the dialog.
    hooks.onHide = [this]()
    {
        if (capabilityPanel)
            capabilityPanel->RevertUnsavedLists();
    };
    hooks.icon = [](const char* name) { return theme::Icon(name); };
    hooks.companionName = [this]() { return DisplayName(); };
    hooks.trayAvailable = []() { return QSystemTrayIcon::isSystemTrayAvailable(); };

    settingsDialog = new desktop::SettingsDialog(backend, std::move(hooks), this);
    settingsDialog->installEventFilter(this);

    // The members the rest of the window reads keep their names; they are the dialog's switches.
    alwaysOnTopCheck = settingsDialog->Switch("window/alwaysOnTop");
    speechCheck = settingsDialog->Switch("speech.enabled");
    autoSendVoiceCheck = settingsDialog->Switch("input/autoSendVoice");
    bargeInCheck = settingsDialog->Switch("bargeIn.enabled");
    handsFreeCheck = settingsDialog->Switch("speechRecognition.handsFree");
    avatarBridgeCheck = settingsDialog->Switch("presence.avatarBridgeEnabled");
    externalAdaptersCheck = settingsDialog->Switch("presence.externalAdaptersEnabled");
    initiativeCheck = settingsDialog->Switch("initiative.enabled");
    curiosityCheck = settingsDialog->Switch("initiative.curiosityEnabled");
    spontaneousSpeechCheck = settingsDialog->Switch("initiative.spontaneousSpeechEnabled");
    speakWhenAwayCheck = settingsDialog->Switch("initiative.speakWhenUserAway");
    aiFilterCheck = settingsDialog->Switch("responseFilter.aiReviewEnabled");

    // Rows without a switch, in the sections their old controls belong to.
    initiativeMaxSpin = new QSpinBox();
    initiativeMaxSpin->setObjectName("initiativeMaxSpin");
    initiativeMaxSpin->setRange(0, 30);
    initiativeMaxSpin->setValue(4);
    initiativeMaxStored = initiativeMaxSpin->value();
    initiativeMaxSpin->setAccessibleName("Unprompted openings per hour");
    settingsDialog->AddCustomRow(SettingsSection::Chat, QStringLiteral("Starting conversations"),
        QStringLiteral("At most N unprompted openings per hour (0 = never)"),
        QStringLiteral("How often {name} may start a conversation without being asked."), initiativeMaxSpin,
        QStringLiteral("initiativeMaxSpinRow"), {"initiative.enabled"});
    connect(initiativeMaxSpin, &QSpinBox::editingFinished, this,
        [this]() { WriteSpinPreference(initiativeMaxSpin, "initiative.maxPerHour", initiativeMaxStored); });

    auto* hardFilterStatus = new QLabel(QStringLiteral("Always on"));
    hardFilterStatus->setObjectName("hardFilterStatus");
    settingsDialog->AddCustomRow(SettingsSection::Chat, QStringLiteral("Reply checks"), QStringLiteral("Basic safety filter"),
        QStringLiteral("Every reply passes the hard safety and grounding filter. It cannot be turned off."), hardFilterStatus,
        QStringLiteral("hardFilterStatusRow"));

    settingsDialog->AddCustomRow(SettingsSection::Voice, QStringLiteral("Listening"), QStringLiteral("Microphone"),
        QStringLiteral("The recording device {name} listens with. Default follows Windows."), microphoneRow,
        QStringLiteral("microphoneDeviceRow"));
    settingsDialog->AddLinkRow(SettingsSection::Voice, QStringLiteral("Listening"), QStringLiteral("Voice generation device"),
        QStringLiteral("Which processor generates the voice is chosen in the Resources window."), QStringLiteral("Open Resources"),
        [this]() { OpenPanel(PanelId::Resources); }, QStringLiteral("voiceDeviceLinkRow"));
    settingsDialog->AddLinkRow(SettingsSection::Vision, QStringLiteral("Pictures"), QStringLiteral("Camera snapshots"),
        QStringLiteral("Camera access is a permission, set under Permissions."), QStringLiteral("Open Permissions"),
        [this]() { settingsDialog->ShowSection(SettingsSection::Permissions); }, QStringLiteral("cameraSnapshotsLinkRow"));

    cloudUseForCombo = new QComboBox();
    cloudUseForCombo->setObjectName("cloudUseForCombo");
    cloudUseForCombo->setAccessibleName("Use the cloud for");
    cloudUseForCombo->addItem(QStringLiteral("Never"), QStringLiteral("never"));
    cloudUseForCombo->addItem(QStringLiteral("Only when I ask"), QStringLiteral("ask"));
    cloudUseForCombo->addItem(QStringLiteral("Hard questions"), QStringLiteral("hardTurns"));
    settingsDialog->AddCustomRow(SettingsSection::Models, QStringLiteral("Cloud"), QStringLiteral("Use the cloud for"),
        QStringLiteral("When {name} may send a question to the cloud model."), cloudUseForCombo, QStringLiteral("cloudUseForComboRow"),
        {"llm.cloud.enabled"});
    connect(cloudUseForCombo, &QComboBox::activated, this,
        [this, backend](const int index)
        {
            if (!backend.writeChoice || index < 0)
                return;
            const QString choice = cloudUseForCombo->itemData(index).toString();
            const auto write = [this, backend, choice]()
            {
                cloudUseForCombo->setEnabled(false);
                backend.writeChoice("llm.cloud.useFor", choice,
                    [this](const desktop::SettingWrite result)
                    {
                        cloudUseForCombo->setEnabled(true);
                        // What was saved, whether the write took or not.
                        RefreshCloudUseFor();
                        ShowStatus(result.message, result.succeeded ? 5000 : 8000);
                    });
            };
            const QString saved = cloudUseForReader ? cloudUseForReader("llm.cloud.useFor") : QString();
            if (choice == QStringLiteral("hardTurns") && saved != choice)
            {
                // Hard questions go to the cloud without being asked for each time, which the
                // question on Allow the cloud model never covered; the saved choice stays shown
                // until the answer.
                RefreshCloudUseFor();
                AskBeforeRiskyChoice(settingsDialog, QStringLiteral("Use the cloud for hard questions?"),
                    QStringLiteral("Questions %1 judges hard go to the cloud model without you asking each time, with the "
                                   "context that goes with them.")
                        .arg(DisplayName()),
                    write);
                return;
            }
            write();
        });
    cloudUseForReader = backend.readChoice;

    resourceSampleSpin = new QSpinBox();
    resourceSampleSpin->setObjectName("resourceSampleSpin");
    resourceSampleSpin->setRange(0, 3600);
    resourceSampleSpin->setValue(2);
    resourceSampleStored = resourceSampleSpin->value();
    resourceSampleSpin->setSpecialValueText(QStringLiteral("Off"));
    resourceSampleSpin->setSuffix(QStringLiteral(" seconds"));
    resourceSampleSpin->setAccessibleName("Resource sampling interval");
    settingsDialog->AddCustomRow(SettingsSection::Advanced, QStringLiteral("Diagnostics"),
        QStringLiteral("Sample resource usage every N seconds (0 = off)"),
        QStringLiteral("How often processor, memory and GPU use are measured for the Resources window."), resourceSampleSpin,
        QStringLiteral("resourceSampleSpinRow"));
    connect(resourceSampleSpin, &QSpinBox::editingFinished, this,
        [this]() { WriteSpinPreference(resourceSampleSpin, "resources.usageSampleSeconds", resourceSampleStored); });

    // Permissions hosts the whole CapabilityPanel; its internet cards go to Internet.
    settingsDialog->SetHostedWidget(SettingsSection::Permissions, QStringLiteral("permissions"), capabilityPanel, 0);
    settingsDialog->SetHostedWidget(SettingsSection::Internet, QStringLiteral("internet"), capabilityPanel->DetachInternetGroup(), 0);
    settingsDialog->SetResizeHook([this](const QSize contentSize) { ApplySettingsLayout(contentSize.width()); });
    settingsDialog->SetCompanionName(DisplayName());

    preferenceRefreshTimer = new QTimer(this);
    preferenceRefreshTimer->setSingleShot(true);
    preferenceRefreshTimer->setInterval(100);
    connect(preferenceRefreshTimer, &QTimer::timeout, this,
        [this]()
        {
            if (switchingCompanion)
                return;
            if (settingsDialog)
                settingsDialog->Refresh();
            RefreshPreferenceSpins();
            RefreshCloudUseFor();
        });
    capabilityRefreshTimer = new QTimer(this);
    capabilityRefreshTimer->setSingleShot(true);
    capabilityRefreshTimer->setInterval(100);
    connect(capabilityRefreshTimer, &QTimer::timeout, this,
        [this]()
        {
            if (capabilityPanel && !switchingCompanion)
                capabilityPanel->Refresh();
        });

    // Shell rows the window acts on at startup.
    QSettings shell;
    chatTextSize = shell.value("appearance/chatTextSize", 15).toInt();
    if (chatTextSize != 14 && chatTextSize != 17)
        chatTextSize = 15;
    showWorkSummaries = shell.value("chat/showWorkSummaries", true).toBool();
    if (shell.value("window/alwaysOnTop", false).toBool())
        SetAlwaysOnTop(true);
    RefreshCloudUseFor();
}

void ReviaWindow::RefreshCloudUseFor()
{
    if (cloudUseForCombo == nullptr || !cloudUseForReader)
        return;
    const QString current = cloudUseForReader("llm.cloud.useFor");
    const int index = cloudUseForCombo->findData(current);
    const QSignalBlocker blocker(cloudUseForCombo);
    cloudUseForCombo->setCurrentIndex(index < 0 ? 0 : index);
}

void ReviaWindow::SchedulePreferenceRefresh()
{
    if (preferenceRefreshTimer)
        preferenceRefreshTimer->start();
}

void ReviaWindow::RefreshSettingsExtras()
{
    if (switchingCompanion)
        return;
    RefreshPreferenceSpins();
    RefreshCloudUseFor();
    // /internet, /set and capability changes from elsewhere reach the hosted panel here, so a
    // switch never shows a value from before them.
    if (capabilityPanel)
        capabilityPanel->Refresh();
}

void ReviaWindow::RefreshPreferenceSpins()
{
    const revia::runtime::UserPreferenceSnapshot snapshot = Session().UserPreferences();
    const auto show = [](QSpinBox* spin, const int value, int& stored)
    {
        // A box being edited keeps what is typed in it; leaving it compares against this read.
        stored = value;
        if (spin == nullptr || spin->hasFocus())
            return;
        const QSignalBlocker blocker(spin);
        spin->setValue(value);
    };
    show(initiativeMaxSpin, snapshot.initiativeMaxPerHour, initiativeMaxStored);
    show(resourceSampleSpin, snapshot.resourceSampleSeconds, resourceSampleStored);
}

void ReviaWindow::WriteSpinPreference(QSpinBox* spin, const std::string& key, int& stored)
{
    const int value = spin->value();
    if (value == stored || switchingCompanion || shuttingDown.load() || !settingsWorker)
        return;
    // Counted as stored now, so leaving the box again while the write runs is not another edit.
    stored = value;
    revia::runtime::ReviaSession* session = &Session();
    const std::uint64_t epoch = sessionUiEpoch.load();
    // On the settings worker, like every other Settings write: the live applier restarts loops,
    // and a background write may hold the preference lock for a while.
    settingsWorker->Post(
        [this, session, key, value, epoch]()
        {
            revia::core::PreferenceResult result;
            try
            {
                result = session->SetPreference(key, std::to_string(value));
            }
            catch (const std::exception& error)
            {
                result = {false, error.what()};
            }
            QMetaObject::invokeMethod(
                this,
                [this, result, epoch]()
                {
                    if (epoch != sessionUiEpoch.load() || shuttingDown.load())
                        return;
                    ShowPreferenceResult(result);
                    if (!result.succeeded)
                        RefreshPreferenceSpins();
                },
                Qt::QueuedConnection);
        });
}

void ReviaWindow::ApplyShellSetting(const std::string& key, const bool value)
{
    if (key == "window/alwaysOnTop")
    {
        SetAlwaysOnTop(value);
        if (keepOnTopAction)
        {
            const QSignalBlocker blocker(keepOnTopAction);
            keepOnTopAction->setChecked(value);
        }
    }
    else if (key == "sidebar/expanded")
    {
        if (navigationSidebar)
            navigationSidebar->SetExpanded(value);
    }
    else if (key == "chat/sidebarVisible")
    {
        if (chatSessionsPanel)
            chatSessionsPanel->SetListVisible(value);
    }
    else if (key == "chat/showWorkSummaries")
    {
        showWorkSummaries = value;
        RenderChat();
    }
    else if (key == "activity/follow")
    {
        const QSignalBlocker blocker(activityAutoScroll);
        activityAutoScroll->setChecked(value);
    }
    else if (key == "speechRecognition.handsFree" || key == "speechRecognition.enabled")
    {
        ApplyMicrophoneUi(microphoneUiState);
    }
}

void ReviaWindow::ApplyChatTextSize(const int pixels)
{
    chatTextSize = pixels == 14 || pixels == 17 ? pixels : 15;
    // Replies and the message box only; the rest of the window keeps its type scale.
    QFont font = messageInput->font();
    font.setPixelSize(chatTextSize);
    messageInput->setFont(font);
    messageInput->setStyleSheet(QStringLiteral("font-size: %1px;").arg(chatTextSize));
    if (composer)
        composer->UpdateInputHeight();
    for (ChatEntry& entry : chatEntries)
        entry.renderedBody.clear();
    RenderChat();
}

void ReviaWindow::ApplyThemeToWindow()
{
    // Markdown HTML bakes colours in, so every cached body is converted again.
    for (ChatEntry& entry : chatEntries)
    {
        entry.renderedSource.clear();
        entry.renderedBody.clear();
    }
    lastTranscriptHtml.clear();
    if (viewingArchivedChat.empty())
    {
        RenderChat();
    }
    else
    {
        std::string error;
        RenderArchivedChat(Session().ConversationTranscript(viewingArchivedChat, error));
    }
    RenderActivity();
    if (chatSessionsPanel)
        chatSessionsPanel->Refresh();
    if (panelHost)
        panelHost->RefreshIcons();
}

void ReviaWindow::ShowStatus(const QString& text, const int timeoutMs)
{
    if (toast)
        toast->ShowStatus(text, timeoutMs);
    else
        statusBar()->showMessage(text, timeoutMs);
}

void ReviaWindow::ShowComposerContext(const QString& context, const QString& actionText, std::function<void()> action)
{
    // The status line keeps the message (what the caller put there); the toast stands down.
    if (toast)
        toast->Clear();
    if (composer)
        composer->SetContext(context, actionText, std::move(action));
}

void ReviaWindow::ShowStillWorking()
{
    statusBar()->showMessage(DisplayName() + QStringLiteral(" is still working on the previous request. Send again once the reply is in."), 8000);
    ShowComposerContext(DisplayName() + QStringLiteral(" is still working on the previous request"), QString(), {});
    composerContextKind = ComposerContextKind::StillWorking;
}

void ReviaWindow::BuildShell()
{
    // A transient-message model only: its text shows as the toast and stays readable through
    // currentMessage(); the bar itself never shows.
    statusBar()->setVisible(false);

    auto* root = new QWidget();
    root->setObjectName("rootPanel");
    root->setAttribute(Qt::WA_StyledBackground, true);
    auto* rootLayout = new QHBoxLayout(root);
    rootLayout->setObjectName("rootLayout");
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    desktop::ShellSidebar::Callbacks callbacks;
    callbacks.newChat = [this]() { chatSessionsPanel->StartNewChat(); };
    callbacks.search = [this]() { commandPalette->OpenChats(); };
    callbacks.settings = [this]() { OpenSettings(); };
    callbacks.toggled = [this](const bool expanded, const bool persisted)
    {
        // A narrow window's temporary expansion is not a preference.
        if (!persisted)
            return;
        QSettings().setValue("sidebar/expanded", expanded);
        if (settingsDialog)
            settingsDialog->SetValue("sidebar/expanded", expanded);
    };
    navigationSidebar = new desktop::ShellSidebar(std::move(callbacks), root);
    navigationSidebar->SetIconProvider(&ShellIcon);
    navigationSidebar->SetBody(chatSessionsPanel);
    navigationSidebar->SetExpanded(QSettings().value("sidebar/expanded", true).toBool());
    rootLayout->addWidget(navigationSidebar);

    BuildChatColumn(root);
    rootLayout->addWidget(chatColumn, 1);

    commandPalette = new desktop::CommandPalette(root);
    commandPalette->SetIconProvider(&ShellIcon);
    commandPalette->SetAvailable([this]() { return !navigationLocked; });
    BuildMenus();
    BuildPalette();
    BuildShortcuts();
    navigationSidebar->SetMenu(sidebarMenu);
    navigationSidebar->SetPanelsAnchor(panelHost->Action(PanelId::Canvas));

    // The .ui's central widget and its parking tab widget go now that every page has moved out.
    // takeCentralWidget, not setCentralWidget alone, which would only delete it later and leave
    // the old tab widgets findable until then.
    QWidget* parking = takeCentralWidget();
    setCentralWidget(root);
    delete parking;

    themeListenerId = theme::AddListener([this]() { ApplyThemeToWindow(); });
    ApplyChatTextSize(chatTextSize);
    UpdateActivitySummary();
    RefreshAccount();
    RefreshChatTitle();
    UpdateConversationStrip();

    // Keyboard order (design spec 9): the sidebar top to bottom, the header, the transcript, the
    // message box, then the composer's toolbar left to right.
    const auto named = [this](const char* name) { return findChild<QWidget*>(QString::fromLatin1(name)); };
    const QList<QWidget*> order = {navigationSidebar->ToggleButton(), named("newChatButton"), named("chatSearch"), chatSidebarToggle,
        named("chatSessionList"), named("sidebarSettingsButton"), named("sidebarMenuButton"), privacyMic, privacyPerception, privacyScreen,
        privacyCamera, privacyDesktop, headerDesktopStop, statePill, runtimeDetailsButton, chatMoreButton, chatHistory, messageInput,
        attachButton, composerProfileChip, chatVoiceHealth->DetailsButton(), composerHandsFreeChip, microphoneButton, stopButton,
        sendButton};
    QWidget* previous = nullptr;
    for (QWidget* widget : order)
    {
        if (widget == nullptr)
            continue;
        if (previous != nullptr)
            setTabOrder(previous, widget);
        previous = widget;
    }
}

void ReviaWindow::BuildChatColumn(QWidget* parent)
{
    chatColumn = new QWidget(parent);
    chatColumn->setObjectName("chatColumn");
    chatColumn->setAttribute(Qt::WA_StyledBackground, true);
    chatColumn->installEventFilter(this);
    auto* column = new QVBoxLayout(chatColumn);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    // Header: the chat's title, the live privacy states, the desktop stop and the status pill.
    chatHeader = new QWidget(chatColumn);
    chatHeader->setObjectName("chatHeader");
    chatHeader->setAttribute(Qt::WA_StyledBackground, true);
    chatHeader->setFixedHeight(48);
    chatHeader->installEventFilter(this);
    auto* header = new QHBoxLayout(chatHeader);
    header->setContentsMargins(16, 8, 16, 8);
    header->setSpacing(8);
    chatTitle = new QLabel(QStringLiteral("New chat"), chatHeader);
    chatTitle->setObjectName("chatTitle");
    chatTitle->setTextFormat(Qt::PlainText);
    chatTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    chatTitle->setMinimumWidth(80);
    header->addWidget(chatTitle, 1);

    privacyIndicators = new QWidget(chatHeader);
    privacyIndicators->setObjectName("privacyIndicators");
    auto* privacy = new QHBoxLayout(privacyIndicators);
    privacy->setContentsMargins(0, 0, 0, 0);
    privacy->setSpacing(4);
    privacyMic = IconButton(privacyIndicators, "privacyMic", "mic:accent", QStringLiteral("Microphone is live"),
        QStringLiteral("Microphone is live: click to stop listening"));
    privacyPerception = IconButton(privacyIndicators, "privacyPerception", "eye:accent", QStringLiteral("App awareness is on"), QString());
    privacyScreen = IconButton(privacyIndicators, "privacyScreen", "monitor:accent", QStringLiteral("Screen awareness is on"), QString());
    privacyCamera = IconButton(privacyIndicators, "privacyCamera", "camera:accent", QStringLiteral("Camera in use"), QStringLiteral("Camera in use"));
    privacyDesktop = IconButton(privacyIndicators, "privacyDesktop", "cursor:danger", QStringLiteral("Desktop control is active"), QString());
    for (QToolButton* button : {privacyMic, privacyPerception, privacyScreen, privacyCamera, privacyDesktop})
    {
        privacy->addWidget(button);
        button->hide();
    }
    connect(privacyMic, &QToolButton::clicked, this, [this]() { ToggleListening(); });
    connect(privacyPerception, &QToolButton::clicked, this,
        [this]()
        {
            Session().SetPerceptionPaused(true);
            ShowStatus(QStringLiteral("App awareness is paused. Change it in Settings › Vision."), 5000);
        });
    connect(privacyScreen, &QToolButton::clicked, this,
        [this]()
        {
            // Through Settings' own switch, so the write takes the backend's path and the row
            // shows it.
            if (QCheckBox* awareness = settingsDialog->Switch("vision.continuousAwareness"); awareness != nullptr && awareness->isEnabled())
                awareness->setChecked(false);
            ShowStatus(QStringLiteral("Turned off. Change it in Settings › Vision."), 5000);
        });
    connect(privacyDesktop, &QToolButton::clicked, this, [this]() { StopDesktopControlFromShell(); });
    header->addWidget(privacyIndicators);

    headerDesktopStop = new QPushButton(QStringLiteral("Stop"), chatHeader);
    headerDesktopStop->setObjectName("headerDesktopStop");
    headerDesktopStop->setToolTip(QStringLiteral("Stop desktop control (or hold Ctrl+Alt+Shift)"));
    headerDesktopStop->setAccessibleName(QStringLiteral("Stop desktop control"));
    headerDesktopStop->setAutoDefault(false);
    headerDesktopStop->hide();
    connect(headerDesktopStop, &QPushButton::clicked, this, [this]() { StopDesktopControlFromShell(); });
    header->addWidget(headerDesktopStop);

    statePill = new QToolButton(chatHeader);
    statePill->setObjectName("statePill");
    statePill->setFocusPolicy(Qt::TabFocus);
    statePill->setAutoRaise(false);
    auto* pill = new QHBoxLayout(statePill);
    pill->setContentsMargins(10, 4, 10, 4);
    pill->setSpacing(6);
    stateDot = new QLabel(statePill);
    stateDot->setObjectName("stateDot");
    stateDot->setFixedSize(8, 8);
    stateDot->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    pill->addWidget(stateDot, 0, Qt::AlignVCenter);
    stateLabel = new QLabel(QStringLiteral("Offline"), statePill);
    stateLabel->setObjectName("stateLabel");
    stateLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    pill->addWidget(stateLabel, 0, Qt::AlignVCenter);
    connect(statePill, &QToolButton::clicked, this, [this]() { ToggleStatusPopover(); });
    header->addWidget(statePill);

    stateDetailLabel = new QLabel(chatHeader);
    stateDetailLabel->setObjectName("stateDetailLabel");
    stateDetailLabel->setTextFormat(Qt::PlainText);
    stateDetailLabel->setMaximumWidth(280);
    stateDetailLabel->hide();
    header->addWidget(stateDetailLabel);

    runtimeDetailsButton = new QPushButton(QStringLiteral("Details"), chatHeader);
    runtimeDetailsButton->setObjectName("runtimeDetailsButton");
    runtimeDetailsButton->setToolTip(QStringLiteral("Show startup messages and the runtime log folder"));
    runtimeDetailsButton->setAutoDefault(false);
    runtimeDetailsButton->hide();
    connect(runtimeDetailsButton, &QPushButton::clicked, this,
        [this]()
        {
            if (!navigationLocked)
                OpenPanel(PanelId::Activity);
        });
    header->addWidget(runtimeDetailsButton);

    chatMoreButton = IconButton(chatHeader, "chatMoreButton", "more", QStringLiteral("Chat actions"), QStringLiteral("Chat actions"));
    chatMoreButton->setPopupMode(QToolButton::InstantPopup);
    header->addWidget(chatMoreButton);
    column->addWidget(chatHeader);

    // The transcript and the composer, in the existing chat page.
    chatPage->setParent(chatColumn);
    column->addWidget(chatPage, 1);
    chatPage->setVisible(true);
    ui->chatLayout->setContentsMargins(0, 0, 0, 0);
    ui->chatLayout->setSpacing(0);
    ui->chatLayout->setStretchFactor(chatHistory, 1);
    BuildComposer();

    jumpToLatest = new QToolButton(chatColumn);
    jumpToLatest->setObjectName("jumpToLatest");
    jumpToLatest->setText(QStringLiteral("Latest"));
    jumpToLatest->setToolTip(QStringLiteral("Jump to the latest message"));
    jumpToLatest->setAccessibleName(QStringLiteral("Jump to the latest message"));
    jumpToLatest->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    jumpToLatest->setFocusPolicy(Qt::TabFocus);
    theme::BindIcon(jumpToLatest, "arrow-down", 14);
    jumpToLatest->hide();
    connect(jumpToLatest, &QToolButton::clicked, this,
        [this]()
        {
            transcriptPinned = true;
            chatHistory->verticalScrollBar()->setValue(chatHistory->verticalScrollBar()->maximum());
            jumpToLatest->hide();
        });

    toast = new desktop::ToastOverlay(chatColumn);
    toast->SetAnchor(composerShadow);
    toast->Follow(statusBar());

    navigationLockOverlay = new QLabel(chatColumn);
    navigationLockOverlay->setObjectName("navigationLockOverlay");
    navigationLockOverlay->setAlignment(Qt::AlignCenter);
    navigationLockOverlay->setWordWrap(true);
    navigationLockOverlay->hide();

    // The six live chips from the old header, one click away on the status pill.
    statusPopover = new QFrame(this, Qt::Popup);
    statusPopover->setObjectName("statusPopover");
    statusPopover->setAttribute(Qt::WA_StyledBackground, true);
    auto* chips = new QGridLayout(statusPopover);
    chips->setContentsMargins(12, 12, 12, 12);
    chips->setHorizontalSpacing(16);
    chips->setVerticalSpacing(8);
    affectLabel = StatusChip(statusPopover, "affectLabel", QStringLiteral("Affect: Neutral"));
    speechLabel = StatusChip(statusPopover, "speechLabel", QStringLiteral("Voice: Starting"));
    microphoneLabel = StatusChip(statusPopover, "microphoneLabel", QStringLiteral("Mic: Starting"));
    automationLabel = StatusChip(statusPopover, "automationLabel", QStringLiteral("Actions: Ready"));
    visionLabel = StatusChip(statusPopover, "visionLabel", QStringLiteral("Vision: Starting"));
    perceptionLabel = StatusChip(statusPopover, "perceptionLabel", QStringLiteral("Watching: Off"));
    perceptionLabel->setToolTip(QStringLiteral("Ambient window observation is off."));
    const QList<QLabel*> chipLabels = {affectLabel, speechLabel, microphoneLabel, automationLabel, visionLabel, perceptionLabel};
    for (int index = 0; index < chipLabels.size(); ++index)
        chips->addWidget(chipLabels[index], index / 2, index % 2);
    auto* openActivity = new QPushButton(QStringLiteral("Open Activity"), statusPopover);
    openActivity->setObjectName("statusPopoverActivity");
    openActivity->setAutoDefault(false);
    chips->addWidget(openActivity, 3, 0, 1, 2, Qt::AlignLeft);
    connect(openActivity, &QPushButton::clicked, this,
        [this]()
        {
            statusPopover->hide();
            OpenPanel(PanelId::Activity);
        });
}

void ReviaWindow::BuildComposer()
{
    QWidget* page = ui->chatLayout->parentWidget();
    // The .ui's button row goes (its buttons move into the composer below), and so does the
    // message box's place in the page.
    ui->chatLayout->removeItem(ui->chatButtonLayout);
    delete ui->chatButtonLayout;
    ui->chatButtonLayout = nullptr;
    ui->chatLayout->removeWidget(messageInput);

    composerArea = new QWidget(page);
    composerArea->setObjectName("composerArea");
    auto* area = new QVBoxLayout(composerArea);
    area->setContentsMargins(16, 0, 16, 12);
    area->setSpacing(6);
    // Both sit in the transcript's 760px measure (ApplyComposerMeasure sets the side margins).
    conversationStatus->setParent(composerArea);
    area->addWidget(conversationStatus);

    auto* shadow = new desktop::SoftShadowHost(composerArea);
    shadow->setObjectName("composerShadowHost");
    shadow->SetRadius(20);
    shadow->SetBlur(12);
    shadow->SetOffset(QPoint(0, 2));
    composerShadow = shadow;
    composer = new desktop::ComposerCard();
    shadow->SetTarget(composer);
    shadow->installEventFilter(this);
    area->addWidget(shadow);

    composer->SetTray(attachmentTray);
    composer->SetInput(messageInput);
    composer->SetMaxInputLines(8);
    composer->SetMaxInputHeight(200);
    // Below the composer's own filter, which is installed when it takes the box: Enter, Esc
    // and paste reach the window either way.
    messageInput->removeEventFilter(this);
    messageInput->installEventFilter(this);

    attachButton->setText(QString());
    theme::BindIcon(attachButton, "plus");
    attachButton->setFocusPolicy(Qt::TabFocus);
    composer->AddLeading(attachButton);

    composerProfileChip = new QToolButton(composer);
    composerProfileChip->setObjectName("composerProfileChip");
    composerProfileChip->setPopupMode(QToolButton::InstantPopup);
    composerProfileChip->setFocusPolicy(Qt::TabFocus);
    composerProfileChip->setToolTip(QStringLiteral("Profile for this chat"));
    composerProfileChip->setAccessibleName(QStringLiteral("Profile"));
    composer->AddTrailing(composerProfileChip);
    chatVoiceHealth->setParent(composer);
    composer->AddTrailing(chatVoiceHealth);
    composerHandsFreeChip = new QToolButton(composer);
    composerHandsFreeChip->setObjectName("composerHandsFreeChip");
    composerHandsFreeChip->setText(QStringLiteral("Hands-free on ✕"));
    composerHandsFreeChip->setToolTip(QStringLiteral("Hands-free listening is on: click to turn it off"));
    composerHandsFreeChip->setAccessibleName(QStringLiteral("Turn hands-free listening off"));
    composerHandsFreeChip->setFocusPolicy(Qt::TabFocus);
    composerHandsFreeChip->hide();
    connect(composerHandsFreeChip, &QToolButton::clicked, this, [this]() { ToggleListening(); });
    composer->AddTrailing(composerHandsFreeChip);
    for (QPushButton* button : {microphoneButton, stopButton, sendButton})
    {
        button->setText(QString());
        button->setFocusPolicy(Qt::TabFocus);
        button->setAutoDefault(false);
        composer->AddTrailing(button);
    }
    theme::BindIcon(sendButton, "arrow-up:onAccent");
    sendButton->setToolTip(QStringLiteral("Send (Enter) · Shift+Enter for a new line"));
    sendButton->setAccessibleName(QStringLiteral("Send"));
    stopButton->setProperty("primary", false);
    theme::BindIcon(stopButton, "square");
    stopButton->setAccessibleName(QStringLiteral("Stop reply"));
    // Kept as objects with their wiring; the + menu reaches them.
    documentsButton->setParent(composerArea);
    screenActionButton->setParent(composerArea);
    documentsButton->hide();
    screenActionButton->hide();

    ui->chatLayout->addWidget(composerArea);
    attachmentTray->SetChangeHook([this]() { UpdateSendState(); });
}

void ReviaWindow::BuildMenus()
{
    // The sidebar's footer menu: the companion, every panel by group, then the app.
    sidebarMenu = new QMenu(this);
    sidebarMenu->setObjectName("sidebarMenu");
    theme::PolishMenu(sidebarMenu);
    menuNameHeader = sidebarMenu->addAction(DisplayName());
    menuNameHeader->setEnabled(false);
    switchCompanionMenu = sidebarMenu->addMenu(QStringLiteral("Switch companion"));
    theme::PolishMenu(switchCompanionMenu);
    connect(switchCompanionMenu, &QMenu::aboutToShow, this,
        [this]()
        {
            switchCompanionMenu->clear();
            if (companions)
            {
                const std::string active = Session().Paths().Descriptor().id;
                for (const auto& companion : companions->List())
                {
                    QAction* entry = switchCompanionMenu->addAction(QString::fromStdString(companion.displayName));
                    entry->setCheckable(true);
                    entry->setChecked(companion.id == active);
                    const std::string id = companion.id;
                    connect(entry, &QAction::triggered, this, [this, id]() { SwitchCompanion(id); });
                }
                switchCompanionMenu->addSeparator();
            }
            connect(switchCompanionMenu->addAction(QStringLiteral("New companion...")), &QAction::triggered, this, [this]() { CreateCompanion(); });
        });
    for (const char* group : {"Workspace", "Companion", "Studio", "Runtime"})
    {
        // A heading the style cannot drop: Fusion with a stylesheet draws addSection() as a bare
        // separator, which left fifteen panels in one unlabelled list.
        sidebarMenu->addSeparator();
        auto* heading = new QLabel(QString::fromLatin1(group));
        heading->setObjectName("sidebarMenuSection");
        auto* section = new QWidgetAction(sidebarMenu);
        section->setText(QString::fromLatin1(group));
        section->setDefaultWidget(heading);
        section->setEnabled(false);
        sidebarMenu->addAction(section);
        for (const desktop::PanelSpec& spec : desktop::PanelCatalog())
        {
            if (QLatin1String(spec.group) == QLatin1String(group))
                sidebarMenu->addAction(panelHost->Action(spec.id));
        }
    }
    sidebarMenu->addSeparator();
    menuStopDesktop = sidebarMenu->addAction(QStringLiteral("Stop desktop control"));
    theme::BindIcon(menuStopDesktop, "cursor:danger");
    menuStopDesktop->setVisible(false);
    connect(menuStopDesktop, &QAction::triggered, this, [this]() { StopDesktopControlFromShell(); });

    // These carry the application shortcuts, so the menu shows them and they work while it is closed.
    const auto appAction = [this](const QString& text, const QKeySequence& keys, std::function<void()> run)
    {
        auto* action = new QAction(text, this);
        action->setShortcut(keys);
        action->setShortcutContext(Qt::ApplicationShortcut);
        addAction(action);
        connect(action, &QAction::triggered, this, [run = std::move(run)]() { run(); });
        return action;
    };
    sidebarMenu->addAction(appAction(QStringLiteral("Settings..."), QKeySequence(Qt::CTRL | Qt::Key_Comma), [this]() { OpenSettings(); }));
    sidebarMenu->addAction(appAction(QStringLiteral("Command palette..."), QKeySequence(Qt::CTRL | Qt::Key_K),
        [this]()
        {
            ShowChat();
            commandPalette->Open();
        }));
    sidebarMenu->addAction(appAction(QStringLiteral("Keyboard shortcuts..."), QKeySequence(Qt::CTRL | Qt::Key_Slash),
        [this]()
        {
            if (!shortcutSheet)
                shortcutSheet = new desktop::ShortcutSheet(ShortcutRows(), this);
            shortcutSheet->show();
            shortcutSheet->raise();
            shortcutSheet->activateWindow();
        }));
    keepOnTopAction = sidebarMenu->addAction(QStringLiteral("Keep Revia on top"));
    keepOnTopAction->setCheckable(true);
    keepOnTopAction->setChecked(QSettings().value("window/alwaysOnTop", false).toBool());
    connect(keepOnTopAction, &QAction::toggled, this,
        [this](const bool enabled)
        {
            if (alwaysOnTopCheck && alwaysOnTopCheck->isChecked() != enabled)
                alwaysOnTopCheck->setChecked(enabled);
        });
    connect(sidebarMenu->addAction(QStringLiteral("Quit Revia")), &QAction::triggered, this,
        [this]() { BeginShutdown(revia::core::ExitReason::UserCommand, "Quit Revia from the menu"); });
    connect(sidebarMenu, &QMenu::aboutToShow, this, [this]() { RefreshSidebarMenu(); });

    // The chat header's menu acts on the current chat.
    chatMoreMenu = new QMenu(this);
    chatMoreMenu->setObjectName("chatMoreMenu");
    theme::PolishMenu(chatMoreMenu);
    const auto current = [this]() { return Session().CurrentConversationId(); };
    connect(chatMoreMenu->addAction(QStringLiteral("Rename chat")), &QAction::triggered, this,
        [this, current]() { chatSessionsPanel->RenameChat(current()); });
    connect(chatMoreMenu->addAction(QStringLiteral("Export as Markdown")), &QAction::triggered, this,
        [this, current]() { chatSessionsPanel->ExportChat(current(), false); });
    connect(chatMoreMenu->addAction(QStringLiteral("Export as JSON")), &QAction::triggered, this,
        [this, current]() { chatSessionsPanel->ExportChat(current(), true); });
    connect(chatMoreMenu->addAction(QStringLiteral("Delete chat")), &QAction::triggered, this,
        [this, current]() { chatSessionsPanel->DeleteChat(current()); });
    chatMoreMenu->addSeparator();
    connect(chatMoreMenu->addAction(QStringLiteral("Documents...")), &QAction::triggered, this, [this]() { ShowDocuments(); });
    QAction* review = chatMoreMenu->addAction(QStringLiteral("Review last answer"));
    connect(review, &QAction::triggered, this, [this]() { ReviewDisplayedAnswer(); });
    connect(chatMoreMenu, &QMenu::aboutToShow, this,
        [this, review]() { review->setEnabled(conversationStatus && conversationStatus->ReviewButton()->isEnabled()); });
    chatMoreButton->setMenu(chatMoreMenu);

    // The composer's +: files, documents and the screen.
    composerAddMenu = new QMenu(this);
    composerAddMenu->setObjectName("composerAddMenu");
    theme::PolishMenu(composerAddMenu);
    QAction* attachFiles = composerAddMenu->addAction(QStringLiteral("Attach files…"));
    theme::BindIcon(attachFiles, "paperclip");
    connect(attachFiles, &QAction::triggered, this, [this]() { ChooseAttachments(); });
    QAction* documents = composerAddMenu->addAction(QStringLiteral("Documents…"));
    theme::BindIcon(documents, "file-text");
    connect(documents, &QAction::triggered, this, [this]() { documentsButton->click(); });
    useScreenAction = composerAddMenu->addAction(QStringLiteral("Use screen…"));
    theme::BindIcon(useScreenAction, "monitor");
    connect(useScreenAction, &QAction::triggered, this, [this]() { screenActionButton->click(); });
    connect(composerAddMenu, &QMenu::aboutToShow, this, [this]() { useScreenAction->setEnabled(screenActionButton->isEnabled()); });
    attachButton->setMenu(composerAddMenu);
    attachButton->setToolTip(QStringLiteral("Add files, documents or screen"));
    attachButton->setAccessibleName(QStringLiteral("Add files, documents or screen"));

    // The profile chip: switch profiles, the session's cloud gate, and the Profiles window.
    composerProfileMenu = new QMenu(this);
    composerProfileMenu->setObjectName("composerProfileMenu");
    theme::PolishMenu(composerProfileMenu);
    connect(composerProfileMenu, &QMenu::aboutToShow, this, [this]() { RefreshProfileMenu(); });
    composerProfileChip->setMenu(composerProfileMenu);
}

void ReviaWindow::RefreshSidebarMenu()
{
    menuNameHeader->setText(DisplayName());
    menuStopDesktop->setVisible(DesktopControlActive());
    const QSignalBlocker blocker(keepOnTopAction);
    keepOnTopAction->setChecked(alwaysOnTopCheck != nullptr && alwaysOnTopCheck->isChecked());
}

void ReviaWindow::RefreshProfileMenu()
{
    composerProfileMenu->clear();
    const revia::runtime::ProfileStudioSnapshot studio = Session().ProfileStudio();
    auto* profiles = new QActionGroup(composerProfileMenu);
    for (const revia::runtime::ProfileSummary& profile : studio.profiles)
    {
        QAction* entry = composerProfileMenu->addAction(QString::fromStdString(profile.displayName.empty() ? profile.id : profile.displayName));
        entry->setCheckable(true);
        entry->setChecked(profile.id == studio.activeProfileId);
        profiles->addAction(entry);
        const std::string id = profile.id;
        connect(entry, &QAction::triggered, this,
            [this, id]()
            {
                const revia::runtime::ProfileOperationResult result = Session().ActivateProfile(id);
                if (profilePanel)
                    profilePanel->Refresh();
                RefreshAccount();
                ShowStatus(QString::fromStdString(result.message), result.succeeded ? 5000 : 8000);
            });
    }
    composerProfileMenu->addSeparator();
    // The session's cloud gate, only while the cloud is allowed. It shows the session's own state
    // (a /cloud command may have changed it since Settings last read it) and writes through
    // Settings' switch, so the row and the menu cannot disagree.
    QCheckBox* cloudSession = settingsDialog ? settingsDialog->Switch("cloud.session") : nullptr;
    QCheckBox* cloudAllowed = settingsDialog ? settingsDialog->Switch("llm.cloud.enabled") : nullptr;
    if (cloudSession != nullptr && cloudAllowed != nullptr && cloudAllowed->isChecked())
    {
        QAction* cloud = composerProfileMenu->addAction(QStringLiteral("Use the cloud this session"));
        cloud->setCheckable(true);
        cloud->setChecked(Session().IsCloudSessionEnabled());
        cloud->setEnabled(cloudSession->isEnabled());
        connect(cloud, &QAction::toggled, this,
            [this, cloudSession](const bool enabled)
            {
                settingsDialog->SetValue("cloud.session", Session().IsCloudSessionEnabled());
                if (cloudSession->isChecked() != enabled)
                    cloudSession->setChecked(enabled);
            });
    }
    connect(composerProfileMenu->addAction(QStringLiteral("Manage profiles…")), &QAction::triggered, this,
        [this]() { OpenPanel(PanelId::Profiles); });
}

void ReviaWindow::RefreshAccount()
{
    const revia::runtime::ProfileStudioSnapshot studio = Session().ProfileStudio();
    const QString profile = QString::fromStdString(studio.activeDisplayName);
    if (navigationSidebar)
        navigationSidebar->SetAccount(DisplayName(), profile);
    if (composerProfileChip)
    {
        composerProfileChip->setText((profile.isEmpty() ? QStringLiteral("Profile") : profile) + QStringLiteral(" ▾"));
        composerProfileChip->setAccessibleName(QStringLiteral("Profile: ") + (profile.isEmpty() ? QStringLiteral("none") : profile));
    }
    if (menuNameHeader)
        menuNameHeader->setText(DisplayName());
    RefreshStateBadge();
}

void ReviaWindow::RefreshChatTitle()
{
    if (chatTitle == nullptr)
        return;
    const QString title = chatSessionsPanel ? chatSessionsPanel->CurrentTitle() : QStringLiteral("New chat");
    chatTitle->setToolTip(title);
    chatTitle->setText(chatTitle->fontMetrics().elidedText(title, Qt::ElideRight, std::max(40, chatTitle->width())));
}

void ReviaWindow::BuildPalette()
{
    using desktop::CommandEntry;
    std::vector<CommandEntry> commands;
    const auto action = [&commands](const QString& id, const QString& title, const QString& keys, const char* icon, std::function<void()> run,
                            std::function<bool()> enabled = {})
    {
        CommandEntry entry;
        entry.id = QStringLiteral("action:") + id;
        entry.title = title;
        entry.group = QStringLiteral("Actions");
        entry.shortcutText = keys;
        entry.icon = theme::Icon(icon);
        entry.run = std::move(run);
        entry.enabled = std::move(enabled);
        commands.push_back(std::move(entry));
    };
    action("newChat", "New chat", "Ctrl+N", "plus", [this]() { chatSessionsPanel->StartNewChat(); });
    // The palette closes before running an entry; reopening it waits for that to settle.
    action("searchChats", "Search chats", "Ctrl+F", "search", [this]() { QTimer::singleShot(0, this, [this]() { commandPalette->OpenChats(); }); });
    action("toggleSidebar", "Toggle sidebar", "Ctrl+B", "panel-left", [this]() { navigationSidebar->ToggleButton()->click(); });
    action("settings", "Settings", "Ctrl+,", "settings", [this]() { OpenSettings(); });
    action("shortcuts", "Keyboard shortcuts", "Ctrl+/", "keyboard",
        [this]()
        {
            if (!shortcutSheet)
                shortcutSheet = new desktop::ShortcutSheet(ShortcutRows(), this);
            shortcutSheet->show();
            shortcutSheet->raise();
            shortcutSheet->activateWindow();
        });
    const auto themeAction = [&action, this](const char* id, const char* title, const char* icon, const theme::Preference preference)
    {
        action(id, title, QString(), icon,
            [this, preference]()
            {
                theme::SetPreference(preference);
                if (settingsDialog)
                    settingsDialog->Refresh();
            });
    };
    themeAction("themeSystem", "Theme: System", "laptop", theme::Preference::System);
    themeAction("themeLight", "Theme: Light", "sun", theme::Preference::Light);
    themeAction("themeDark", "Theme: Dark", "moon", theme::Preference::Dark);
    action("listen", "Start or stop listening", "Ctrl+Space", "mic", [this]() { ToggleListening(); },
        [this]() { return microphoneButton->isEnabled(); });
    action("attach", "Attach files", QString(), "paperclip", [this]() { ChooseAttachments(); });
    action("useScreen", "Use screen", QString(), "monitor", [this]() { screenActionButton->click(); },
        [this]() { return screenActionButton->isEnabled(); });
    action("documents", "Documents", QString(), "file-text", [this]() { ShowDocuments(); });
    action("keepOnTop", "Keep Revia on top", QString(), "check",
        [this]()
        {
            if (alwaysOnTopCheck)
                alwaysOnTopCheck->setChecked(!alwaysOnTopCheck->isChecked());
        });
    action("stopDesktop", "Stop desktop control", QString(), "cursor:danger", [this]() { StopDesktopControlFromShell(); },
        [this]() { return DesktopControlActive(); });
    action("quit", "Quit Revia", QString(), "x", [this]() { BeginShutdown(revia::core::ExitReason::UserCommand, "Quit Revia from the palette"); });

    for (const desktop::PanelSpec& spec : desktop::PanelCatalog())
    {
        const QString title = QString::fromUtf8(spec.title);
        const PanelId id = spec.id;
        CommandEntry open;
        open.id = QStringLiteral("panel:") + QString::fromLatin1(spec.key);
        open.title = QStringLiteral("Open ") + title;
        open.subtitle = QStringLiteral("Window");
        open.group = QStringLiteral("Panels");
        open.shortcutText = spec.shortcut.toString(QKeySequence::NativeText);
        open.icon = theme::Icon(spec.iconName);
        open.keywords = {QString::fromLatin1(spec.key), QString::fromLatin1(spec.group)};
        open.run = [this, id]() { OpenPanel(id); };
        // A panel's name ranks it above a setting that merely mentions it.
        open.priority = 1;
        commands.push_back(open);
        CommandEntry beside;
        beside.id = QStringLiteral("beside:") + QString::fromLatin1(spec.key);
        beside.title = QStringLiteral("Place ") + title + QStringLiteral(" beside chat");
        beside.subtitle = QStringLiteral("Window");
        beside.group = QStringLiteral("Panels");
        beside.icon = theme::Icon("columns");
        beside.run = [this, id]() { panelHost->PlaceBeside(id); };
        beside.priority = -1;
        commands.push_back(beside);
    }

    const QString name = DisplayName();
    for (const desktop::SettingDescriptor& descriptor : desktop::SettingCatalog())
    {
        if (desktop::IsServerTemplate(descriptor))
            continue;
        CommandEntry setting;
        setting.id = QStringLiteral("setting:") + QString::fromStdString(descriptor.key);
        setting.title = desktop::WithCompanionName(descriptor.label, name);
        setting.subtitle = QStringLiteral("Settings › ") + desktop::SectionName(descriptor.section);
        setting.group = QStringLiteral("Settings");
        setting.icon = theme::Icon(desktop::SectionIcon(descriptor.section));
        setting.keywords = descriptor.keywords;
        setting.keywords << desktop::WithCompanionName(descriptor.description, name);
        const QString key = QString::fromStdString(descriptor.key);
        setting.run = [this, key]() { OpenSettings(key); };
        commands.push_back(std::move(setting));
    }

    if (companions)
    {
        const std::string active = Session().Paths().Descriptor().id;
        for (const auto& companion : companions->List())
        {
            if (companion.id == active)
                continue;
            CommandEntry entry;
            entry.id = QStringLiteral("companion:") + QString::fromStdString(companion.id);
            entry.title = QStringLiteral("Switch to ") + QString::fromStdString(companion.displayName);
            entry.group = QStringLiteral("Companions");
            entry.icon = theme::Icon("users");
            const std::string id = companion.id;
            entry.run = [this, id]() { SwitchCompanion(id); };
            commands.push_back(std::move(entry));
        }
    }
    commandPalette->SetCommands(std::move(commands));

    // Chats come from the archive as the query changes.
    commandPalette->SetDynamicSource(
        [this](const QString& query)
        {
            std::vector<CommandEntry> chats;
            std::string error;
            const std::string current = Session().CurrentConversationId();
            for (const revia::memory::ArchivedSession& chat : Session().BrowseConversations(query.trimmed().toStdString(), 8, error))
            {
                CommandEntry entry;
                entry.id = QStringLiteral("chat:") + QString::fromStdString(chat.id);
                QString title = QString::fromStdString(chat.title.empty() ? chat.opening : chat.title).simplified();
                entry.title = title.isEmpty() ? QStringLiteral("Untitled chat") : title;
                bool parsed = false;
                const qint64 started = QString::fromStdString(chat.startedAt).toLongLong(&parsed);
                entry.subtitle = chat.id == current ? QStringLiteral("Current chat")
                                 : parsed && started > 0 ? QDateTime::fromSecsSinceEpoch(started).toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                                                         : QString();
                entry.group = QStringLiteral("Chats");
                entry.icon = theme::Icon("message");
                const std::string id = chat.id;
                entry.run = [this, id]() { OpenArchivedChat(id); };
                chats.push_back(std::move(entry));
            }
            return chats;
        });
}

std::vector<std::pair<QString, QString>> ReviaWindow::ShortcutRows() const
{
    return {
        {QStringLiteral("Window"), QString()},
        {QStringLiteral("New chat"), QStringLiteral("Ctrl+N")},
        {QStringLiteral("Toggle sidebar"), QStringLiteral("Ctrl+B")},
        {QStringLiteral("Search chats (palette while the sidebar is a rail)"), QStringLiteral("Ctrl+F")},
        {QStringLiteral("Start or stop listening"), QStringLiteral("Ctrl+Space")},
        {QStringLiteral("Move between sidebar, transcript and message box"), QStringLiteral("F6 / Shift+F6")},
        {QStringLiteral("Message box"), QString()},
        {QStringLiteral("Send"), QStringLiteral("Enter")},
        {QStringLiteral("New line"), QStringLiteral("Shift+Enter")},
        {QStringLiteral("Cancel the edit, else stop"), QStringLiteral("Esc")},
        {QStringLiteral("Chat list"), QString()},
        {QStringLiteral("Rename the current chat"), QStringLiteral("F2")},
        {QStringLiteral("Delete the current chat"), QStringLiteral("Delete")},
        {QStringLiteral("Anywhere"), QString()},
        {QStringLiteral("Command palette"), QStringLiteral("Ctrl+K")},
        {QStringLiteral("Settings"), QStringLiteral("Ctrl+,")},
        {QStringLiteral("Keyboard shortcuts"), QStringLiteral("Ctrl+/")},
        {QStringLiteral("Activity window"), QStringLiteral("Ctrl+Shift+A")},
        {QStringLiteral("Memory window"), QStringLiteral("Ctrl+Shift+M")},
        {QStringLiteral("Panel windows"), QString()},
        {QStringLiteral("Close the window"), QStringLiteral("Ctrl+W")},
        {QStringLiteral("Close, except while editing text"), QStringLiteral("Esc")},
        {QStringLiteral("Settings"), QString()},
        {QStringLiteral("Search settings"), QStringLiteral("Ctrl+F")},
        {QStringLiteral("Clear the search, then close"), QStringLiteral("Esc")},
    };
}

void ReviaWindow::BuildShortcuts()
{
    const auto shortcut = [this](const QKeySequence& keys, std::function<void()> run)
    {
        auto* key = new QShortcut(keys, this);
        key->setContext(Qt::WindowShortcut);
        connect(key, &QShortcut::activated, this,
            [this, run = std::move(run)]()
            {
                if (!navigationLocked)
                    run();
            });
        return key;
    };
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_N), [this]() { chatSessionsPanel->StartNewChat(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_B), [this]() { navigationSidebar->ToggleButton()->click(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_F),
        [this]()
        {
            // The rail has no search box; the palette searches chats instead.
            if (navigationSidebar->IsExpanded())
            {
                if (auto* search = chatSessionsPanel->findChild<QLineEdit*>("chatSearch"))
                {
                    search->setFocus(Qt::ShortcutFocusReason);
                    search->selectAll();
                }
            }
            else
            {
                commandPalette->OpenChats();
            }
        });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Space),
        [this]()
        {
            if (microphoneButton->isEnabled())
                ToggleListening();
        });
    // F6 moves between the three areas, as Windows apps do.
    const auto cycle = [this](const int step)
    {
        QWidget* sidebarTarget = navigationSidebar->IsExpanded() ? static_cast<QWidget*>(chatSessionsPanel->findChild<QListWidget*>("chatSessionList"))
                                                                 : static_cast<QWidget*>(navigationSidebar->ToggleButton());
        const QList<QWidget*> areas = {sidebarTarget, chatHistory, messageInput};
        int at = -1;
        for (int index = 0; index < areas.size(); ++index)
        {
            if (areas[index] != nullptr && (areas[index] == focusWidget() || areas[index]->isAncestorOf(focusWidget())))
                at = index;
        }
        const int next = at < 0 ? (step > 0 ? 0 : 2) : (at + step + areas.size()) % areas.size();
        if (areas[next] != nullptr)
            areas[next]->setFocus(Qt::ShortcutFocusReason);
    };
    shortcut(QKeySequence(Qt::Key_F6), [cycle]() { cycle(1); });
    shortcut(QKeySequence(Qt::SHIFT | Qt::Key_F6), [cycle]() { cycle(-1); });
}

void ReviaWindow::SetNavigationLocked(const bool locked)
{
    navigationLocked = locked;
    if (navigationSidebar)
        navigationSidebar->setEnabled(!locked);
    if (chatColumn)
        chatColumn->setEnabled(!locked);
    if (panelHost)
        panelHost->SetLocked(locked);
    if (settingsDialog)
        settingsDialog->SetLocked(locked);
    if (locked && commandPalette)
        commandPalette->Close();
    if (navigationLockOverlay)
    {
        navigationLockOverlay->setText(shuttingDown.load() ? QStringLiteral("Closing Revia…")
                                                           : QStringLiteral("Switching companion…"));
        navigationLockOverlay->setGeometry(chatColumn->rect());
        navigationLockOverlay->setVisible(locked);
        navigationLockOverlay->raise();
    }
}

QList<QWidget*> ReviaWindow::NavigationPages() const
{
    QList<QWidget*> pages{chatPage};
    if (panelHost)
    {
        for (QWidget* page : panelHost->Pages())
        {
            // A page that keeps tabs of its own (Mind) is reached view by view.
            const QList<QTabWidget*> tabs = page->findChildren<QTabWidget*>();
            if (tabs.isEmpty())
            {
                pages.push_back(page);
                continue;
            }
            for (QTabWidget* group : tabs)
            {
                for (int index = 0; index < group->count(); ++index)
                {
                    QWidget* view = group->widget(index);
                    if (qobject_cast<QTabWidget*>(view) == nullptr && view->findChild<QTabWidget*>() == nullptr)
                        pages.push_back(view);
                }
            }
        }
    }
    if (settingsDialog)
        pages.append(settingsDialog->SectionPages());
    return pages;
}

QString ReviaWindow::NavigationTitle(QWidget* page) const
{
    if (page == nullptr)
        return {};
    if (page == chatPage)
        return QStringLiteral("Chat");
    if (settingsDialog)
    {
        for (const SettingsSection section : desktop::SettingsSections())
        {
            if (settingsDialog->SectionPage(section) == page)
                return QStringLiteral("Settings › ") + desktop::SectionName(section);
        }
    }
    if (panelHost)
    {
        if (const auto id = panelHost->IdOf(page))
        {
            const QString title = QString::fromUtf8(desktop::Spec(*id).title);
            if (panelHost->Window(*id) != nullptr && panelHost->Window(*id)->Page() == page)
                return title;
            // One of a panel's own views: the window's title and the view's tab.
            for (QWidget* ancestor = page->parentWidget(); ancestor != nullptr; ancestor = ancestor->parentWidget())
            {
                if (auto* tabs = qobject_cast<QTabWidget*>(ancestor))
                {
                    for (int index = 0; index < tabs->count(); ++index)
                    {
                        if (tabs->widget(index) == page || tabs->widget(index)->isAncestorOf(page))
                            return title + QStringLiteral(" › ") + tabs->tabText(index);
                    }
                }
            }
            return title;
        }
    }
    return page->objectName();
}

void ReviaWindow::OpenSettings(const QString& sectionOrKey)
{
    if (settingsDialog == nullptr)
        return;
    if (!sectionOrKey.isEmpty())
        settingsDialog->ShowSection(sectionOrKey);
    if (settingsDialog->isMinimized())
        settingsDialog->showNormal();
    else
        settingsDialog->show();
    settingsDialog->raise();
    settingsDialog->activateWindow();
}

void ReviaWindow::OpenPanel(const PanelId id)
{
    if (panelHost)
        panelHost->Open(id);
}

void ReviaWindow::ShowChat()
{
    if (isMinimized() || !isVisible())
        showNormal();
    raise();
    activateWindow();
}

QWidget* ReviaWindow::ShowScreenshotTarget(const QString& name)
{
    const QString wanted = name.trimmed();
    if (wanted.isEmpty() || wanted.compare(QStringLiteral("chat"), Qt::CaseInsensitive) == 0)
        return this;
    if (wanted.compare(QStringLiteral("settings"), Qt::CaseInsensitive) == 0)
    {
        OpenSettings();
        return settingsDialog;
    }
    // A bare name means a panel, as it meant a tab before; "settings:voice" names the section
    // that shares a panel's name (Voice, Vision, Memory, Internet).
    const QString settingsPrefix = QStringLiteral("settings:");
    const bool settingsOnly = wanted.startsWith(settingsPrefix, Qt::CaseInsensitive);
    const QString key = settingsOnly ? wanted.mid(settingsPrefix.size()).trimmed() : wanted;
    if (!settingsOnly)
    {
        for (const desktop::PanelSpec& spec : desktop::PanelCatalog())
        {
            if (QString::fromLatin1(spec.key).compare(key, Qt::CaseInsensitive) == 0 ||
                QString::fromUtf8(spec.title).startsWith(key, Qt::CaseInsensitive))
            {
                OpenPanel(spec.id);
                return panelHost->Window(spec.id);
            }
        }
    }
    for (const SettingsSection section : desktop::SettingsSections())
    {
        if (!key.isEmpty() && desktop::SectionName(section).startsWith(key, Qt::CaseInsensitive))
        {
            OpenSettings(desktop::SectionName(section));
            return settingsDialog;
        }
    }
    return nullptr;
}

void ReviaWindow::RefreshPrivacyIndicators()
{
    if (privacyIndicators == nullptr)
        return;
    const QString name = DisplayName();
    privacyMic->setVisible(microphoneUiState == MicrophoneUi::HandsFree || microphoneUiState == MicrophoneUi::Listening ||
                           microphoneUiState == MicrophoneUi::Transcribing);
    privacyPerception->setVisible(perceptionWatching);
    privacyPerception->setToolTip(name + QStringLiteral(" notices which app you're using: click to pause"));
    privacyScreen->setVisible(screenAwarenessRunning);
    privacyScreen->setToolTip(name + QStringLiteral(" keeps track of your screen: click to turn off"));
    privacyCamera->setVisible(cameraCapturing);
    const bool desktopControl = DesktopControlActive();
    privacyDesktop->setVisible(desktopControl);
    privacyDesktop->setToolTip(name + QStringLiteral(" is controlling the mouse and keyboard"));
    headerDesktopStop->setVisible(desktopControl);
    if (menuStopDesktop)
        menuStopDesktop->setVisible(desktopControl);
}

void ReviaWindow::UpdateConversationStrip()
{
    if (conversationStatus == nullptr)
        return;
    using revia::runtime::RuntimeState;
    const bool working = lastRuntimeState != RuntimeState::Idle && lastRuntimeState != RuntimeState::Offline;
    const auto* details = conversationStatus->findChild<QPushButton*>("conversationDetailsButton");
    const auto* quality = conversationStatus->findChild<QLabel*>("conversationQualityDiagnostic");
    // The labels keep updating while it is hidden; only the strip itself comes and goes.
    conversationStatus->setVisible(working || (details != nullptr && details->isChecked()) ||
                                   (quality != nullptr && !quality->text().trimmed().isEmpty()));
}

void ReviaWindow::ToggleStatusPopover()
{
    // While an action waits for the owner's OK, the pill brings that question forward instead.
    if (lastRuntimeState == revia::runtime::RuntimeState::WaitingForConfirmation && openQuestion)
    {
        openQuestion->raise();
        openQuestion->activateWindow();
        return;
    }
    if (statusPopover->isVisible())
    {
        statusPopover->hide();
        return;
    }
    statusPopover->adjustSize();
    const QPoint below = statePill->mapToGlobal(QPoint(statePill->width() - statusPopover->width(), statePill->height() + 4));
    statusPopover->move(below);
    statusPopover->show();
}

void ReviaWindow::StopDesktopControlFromShell()
{
    userStopRequested = true;
    Session().StopDesktopControl("stopped from the chat header");
    if (stopButton->isEnabled())
        stopButton->click();
    RefreshPrivacyIndicators();
    RefreshStateBadge();
}
