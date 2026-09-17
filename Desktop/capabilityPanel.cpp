#include "capabilityPanel.h"
#include "toggleSwitch.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSvgWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace
{
// Row widgets remember which switch they stand for, so the panel's event filter can
// forward a click on the label to it.
constexpr const char* RowTargetProperty = "reviaRowTarget";

QFrame* MakeCard(const QString& objectName)
{
    auto* card = new QFrame();
    card->setObjectName(objectName);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(0);
    return card;
}

// Small inline glyphs rather than a resource file: each is a few hundred bytes, and
// keeping them beside the card they label is easier to follow than a .qrc indirection.
QWidget* MakeIcon(const QString& path, const QString& stroke)
{
    const QString document = QStringLiteral(
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
        "stroke='%1' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'>"
        "%2</svg>").arg(stroke, path);
    auto* icon = new QSvgWidget();
    icon->load(document.toUtf8());
    icon->setFixedSize(18, 18);
    icon->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    return icon;
}

void AddCardHeader(
    QFrame* card,
    const QString& iconPath,
    const QString& stroke,
    const QString& iconObjectName,
    const QString& title,
    const QString& subtitle)
{
    auto* header = new QHBoxLayout();
    header->setSpacing(11);
    auto* well = new QFrame();
    well->setObjectName(iconObjectName);
    well->setFixedSize(32, 32);
    auto* wellLayout = new QVBoxLayout(well);
    wellLayout->setContentsMargins(0, 0, 0, 0);
    wellLayout->addWidget(MakeIcon(iconPath, stroke), 0, Qt::AlignCenter);
    header->addWidget(well, 0, Qt::AlignTop);

    auto* text = new QVBoxLayout();
    text->setSpacing(2);
    auto* titleLabel = new QLabel(title);
    titleLabel->setObjectName("cardTitle");
    text->addWidget(titleLabel);
    auto* subtitleLabel = new QLabel(subtitle);
    subtitleLabel->setObjectName("cardSubtitle");
    subtitleLabel->setWordWrap(true);
    text->addWidget(subtitleLabel);
    header->addLayout(text, 1);

    auto* cardLayout = qobject_cast<QVBoxLayout*>(card->layout());
    cardLayout->addLayout(header);
    cardLayout->addSpacing(14);
}

// One permission: label, optional helper line and risk chip, switch on the right.
QFrame* MakeRow(
    ToggleSwitch* toggle,
    const QString& label,
    const QString& help = QString(),
    const QString& chipText = QString(),
    const QString& chipObjectName = QString(),
    const bool nested = false,
    const QString& tip = QString())
{
    auto* row = new QFrame();
    row->setObjectName(nested ? "permRowNested" : "permRow");
    row->setProperty(RowTargetProperty, QVariant::fromValue<QObject*>(toggle));
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(nested ? 26 : 4, 12, 4, 12);
    layout->setSpacing(16);

    auto* text = new QVBoxLayout();
    text->setSpacing(4);
    auto* labelRow = new QHBoxLayout();
    labelRow->setSpacing(7);
    auto* labelWidget = new QLabel(label);
    labelWidget->setObjectName(nested ? "permLabelNested" : "permLabel");
    labelRow->addWidget(labelWidget);
    if (!chipText.isEmpty())
    {
        auto* chip = new QLabel(chipText);
        chip->setObjectName(chipObjectName);
        labelRow->addWidget(chip);
    }
    if (!tip.isEmpty())
    {
        // The tooltip text already existed; it was only reachable by hovering the
        // checkbox, which gave no sign it was there. The marker makes it discoverable.
        auto* info = new QLabel("i");
        info->setObjectName("infoDot");
        info->setAlignment(Qt::AlignCenter);
        info->setFixedSize(15, 15);
        info->setToolTip(tip);
        labelRow->addWidget(info);
        labelWidget->setToolTip(tip);
    }
    labelRow->addStretch();
    text->addLayout(labelRow);
    if (!help.isEmpty())
    {
        auto* helpLabel = new QLabel(help);
        helpLabel->setObjectName("permHelp");
        helpLabel->setWordWrap(true);
        text->addWidget(helpLabel);
    }
    layout->addLayout(text, 1);
    layout->addWidget(toggle, 0, Qt::AlignVCenter);
    return row;
}

void AddRow(QFrame* card, QFrame* row)
{
    qobject_cast<QVBoxLayout*>(card->layout())->addWidget(row);
}

QFrame* MakeSeparator()
{
    auto* line = new QFrame();
    line->setObjectName("rowSeparator");
    line->setFixedHeight(1);
    return line;
}
}

bool CapabilityPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonRelease)
    {
        auto* row = qobject_cast<QWidget*>(watched);
        const auto* click = static_cast<QMouseEvent*>(event);
        if (row && click->button() == Qt::LeftButton && row->rect().contains(click->pos()))
        {
            // Cast to the QCheckBox base rather than ToggleSwitch: the subclass carries
            // no Q_OBJECT, and toggling is base-class behavior anyway.
            auto* target = qobject_cast<QCheckBox*>(
                row->property(RowTargetProperty).value<QObject*>());
            if (target && target->isEnabled())
            {
                target->toggle();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

CapabilityPanel::CapabilityPanel(
    revia::runtime::ReviaSession& inputSession,
    DiscoveryRequest inputDiscoveryRequest,
    QWidget* parent)
    : QWidget(parent),
      session(inputSession),
      requestDiscovery(std::move(inputDiscoveryRequest))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(16);

    auto* title = new QLabel("Application and internet permissions", this);
    title->setObjectName("sectionTitle");
    layout->addWidget(title);

    auto* intro = new QFrame(this);
    intro->setObjectName("introBar");
    auto* introLayout = new QVBoxLayout(intro);
    introLayout->setContentsMargins(14, 11, 14, 11);
    auto* explanation = new QLabel(
        "Applications begin with read-only inspection permission and no mutable controls. "
        "Discover a foreground window, then approve only the controls Revia may invoke or edit. "
        "Internet access is a separate read-only, rate-limited capability.", intro);
    explanation->setWordWrap(true);
    explanation->setObjectName("secondaryText");
    introLayout->addWidget(explanation);
    layout->addWidget(intro);

    auto* taskApprovals = new QFrame(this);
    auto* taskLayout = new QVBoxLayout(taskApprovals);
    auto* taskTitle = new QLabel("Requested tasks", taskApprovals);
    taskTitle->setObjectName("cardTitle");
    taskLayout->addWidget(taskTitle);
    taskApprovalMode = new QComboBox(taskApprovals);
    taskApprovalMode->setObjectName("taskApprovalMode");
    using ExecutionMode = revia::actions::ExecutionMode;
    taskApprovalMode->addItem("Approve once at the start", static_cast<int>(ExecutionMode::Supervised));
    taskApprovalMode->addItem("Free — use granted permissions without asking", static_cast<int>(ExecutionMode::OwnerFullAccess));
    taskApprovalMode->addItem("Approved scope only", static_cast<int>(ExecutionMode::ApprovedScope));
    taskApprovalMode->addItem("Actions disabled", static_cast<int>(ExecutionMode::Disabled));
    taskLayout->addWidget(taskApprovalMode);
    auto* taskHelp = new QLabel(
        "Task approval covers routine steps and messages you explicitly request. "
        "Free mode skips that initial approval. Existing permissions still apply; "
        "work outside the task's scope stops. Background work gets no task approval.", taskApprovals);
    taskHelp->setWordWrap(true);
    taskHelp->setObjectName("secondaryText");
    taskLayout->addWidget(taskHelp);
    layout->addWidget(taskApprovals);
    connect(taskApprovalMode, &QComboBox::activated, this, [this](const int index)
    {
        if (refreshing) return;
        const auto mode = static_cast<ExecutionMode>(taskApprovalMode->itemData(index).toInt());
        const auto result = session.SetExecutionMode(mode);
        SetStatus(QString::fromStdString(result.message), !result.succeeded);
        Refresh();
    });

    auto* cards = new QGridLayout();
    cards->setHorizontalSpacing(18);
    cards->setVerticalSpacing(18);
    cards->setColumnStretch(0, 1);
    cards->setColumnStretch(1, 1);

    // ------------------------------------------------------------ internet
    auto* internetCard = MakeCard("permCard");
    AddCardHeader(internetCard,
        "<circle cx='12' cy='12' r='9'/><path d='M3 12h18M12 3c2.5 2.6 2.5 15.4 0 18M12 3"
        "c-2.5 2.6-2.5 15.4 0 18'/>",
        "#55E8F2", "iconWell",
        "Internet & Browser",
        "Read-only and rate-limited. Results are grounding, never instructions.");
    internetCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    automaticLookupCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    visibleBrowserCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    autonomousResearchCheck = new ToggleSwitch(ToggleSwitch::Accent::Violet, this);
    AddRow(internetCard, MakeRow(internetCheck,
        "Allow bounded internet lookup",
        "Searches a fixed set of public HTTPS sources on request."));
    AddRow(internetCard, MakeRow(automaticLookupCheck,
        "Automatically look up factual questions",
        "Without this she searches only when you ask her to.",
        QString(), QString(), true));
    AddRow(internetCard, MakeRow(visibleBrowserCheck,
        "Use a dedicated visible browser window",
        QString(), QString(), QString(), true));
    AddRow(internetCard, MakeRow(autonomousResearchCheck,
        "Research her own topics",
        "Lets curiosity start a bounded search with no request from you.",
        "Autonomy", "chipAutonomy", true));
    qobject_cast<QVBoxLayout*>(internetCard->layout())->addStretch();
    cards->addWidget(internetCard, 0, 0);

    // ------------------------------------------------------------ camera
    auto* cameraCard = MakeCard("permCard");
    AddCardHeader(cameraCard,
        "<path d='M3 8.5A2.5 2.5 0 0 1 5.5 6h2L9 4h6l1.5 2h2A2.5 2.5 0 0 1 21 8.5v8A2.5 "
        "2.5 0 0 1 18.5 19h-13A2.5 2.5 0 0 1 3 16.5z'/><circle cx='12' cy='12.5' r='3.4'/>",
        "#55E8F2", "iconWell",
        "Camera",
        "Opened for a single frame, then closed again immediately.");
    cameraCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    autonomousCameraCheck = new ToggleSwitch(ToggleSwitch::Accent::Violet, this);
    AddRow(cameraCard, MakeRow(cameraCheck,
        "Allow camera access",
        "Still frames only. No continuous capture, no recording.",
        QString(), QString(), false,
        "Revia may take a single still frame when you ask her to look at something. "
        "The camera is opened for that frame and closed again immediately."));
    AddRow(cameraCard, MakeRow(autonomousCameraCheck,
        "Let her decide when to look",
        QString(), "Autonomy", "chipAutonomy", true,
        "A separate permission. Without it she can only use the camera when asked."));
    qobject_cast<QVBoxLayout*>(cameraCard->layout())->addStretch();
    cards->addWidget(cameraCard, 0, 1);

    // ------------------------------------------------------------ pointer and keyboard
    auto* desktopCard = MakeCard("permCard");
    AddCardHeader(desktopCard,
        "<path d='M5 3.5 18.5 11l-5.8 1.6L10 19z'/>",
        "#55E8F2", "iconWell",
        "Pointer & Keyboard",
        "Synthesized input is indistinguishable from you typing. Confined to approved "
        "applications unless that confinement is lifted.");
    pointerCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    keyboardCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    launchCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    rawCoordinateCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    visualTargetCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    AddRow(desktopCard, MakeRow(pointerCheck, "Move and click the pointer"));
    AddRow(desktopCard, MakeRow(keyboardCheck, "Type and press key chords"));
    AddRow(desktopCard, MakeRow(launchCheck, "Start approved applications"));
    AddRow(desktopCard, MakeRow(visualTargetCheck,
        "Click what she can see",
        "For games and drawn interfaces that expose no controls to Windows.",
        QString(), QString(), true,
        "The target must be something she just looked at, in the window she looked at, "
        "still in front and still the same size. Anything else is refused and she looks "
        "again."));
    AddRow(desktopCard, MakeRow(rawCoordinateCheck,
        "Aim at coordinates she chose",
        "Otherwise every click must land on a re-verified element.",
        QString(), QString(), true,
        "Wider than the setting above: this is a point with nothing behind it, rather "
        "than a thing she was looking at."));

    auto* desktopLayout = qobject_cast<QVBoxLayout*>(desktopCard->layout());
    desktopLayout->addSpacing(6);
    auto* stopBar = new QFrame();
    stopBar->setObjectName("stopBar");
    auto* stopLayout = new QHBoxLayout(stopBar);
    stopLayout->setContentsMargins(11, 9, 11, 9);
    stopLayout->setSpacing(10);
    auto* stopState = new QLabel("Hold ctrl+alt+shift to halt input immediately.");
    stopState->setObjectName("stopState");
    stopState->setWordWrap(true);
    stopLayout->addWidget(stopState, 1);
    desktopStopButton = new QPushButton("Stop desktop control", this);
    desktopStopButton->setObjectName("stopButton");
    stopLayout->addWidget(desktopStopButton, 0);
    desktopLayout->addWidget(stopBar);
    desktopLayout->addStretch();
    cards->addWidget(desktopCard, 1, 0);

    // ------------------------------------------------------------ gated
    auto* gatedCard = MakeCard("gatedCard");
    AddCardHeader(gatedCard,
        "<path d='M12 3 4 6.2v5.3c0 4.6 3.2 8.6 8 9.5 4.8-.9 8-4.9 8-9.5V6.2z'/>"
        "<path d='M12 9v4'/><path d='M12 16h.01'/>",
        "#F3A446", "iconWellGated",
        "Gated Controls",
        "Each one removes a boundary the other permissions rely on. Off by default.");
    auto* gatedNotice = new QLabel(
        "These extend Revia beyond approved applications. Policy, confirmation and the "
        "audit log still apply to every action — but the scope they are checked "
        "against grows.");
    gatedNotice->setObjectName("gatedNotice");
    gatedNotice->setWordWrap(true);
    auto* gatedLayout = qobject_cast<QVBoxLayout*>(gatedCard->layout());
    gatedLayout->addWidget(gatedNotice);
    gatedLayout->addSpacing(4);

    wholeDesktopCheck = new ToggleSwitch(ToggleSwitch::Accent::Amber, this);
    commandSurfaceCheck = new ToggleSwitch(ToggleSwitch::Accent::Red, this);
    autonomousDesktopCheck = new ToggleSwitch(ToggleSwitch::Accent::Violet, this);
    AddRow(gatedCard, MakeRow(wholeDesktopCheck,
        "Give her the whole desktop",
        "Allows Revia to act outside approved applications. Requires pointer control "
        "and chosen coordinates.",
        "Removes confinement", "chipGated", false,
        "The pointer goes anywhere and the keyboard goes to whatever has focus, the way "
        "it does for you. Needs pointer control and chosen coordinates."));
    AddRow(gatedCard, MakeSeparator());
    AddRow(gatedCard, MakeRow(commandSurfaceCheck,
        "Allow terminals and script hosts",
        "Typing into a command interpreter is model text reaching a shell, whichever "
        "window it arrived through.",
        "Command surface", "chipCritical", false,
        "Off by default. Typing into a command interpreter is model text reaching a "
        "shell no matter which window it arrived through."));
    AddRow(gatedCard, MakeSeparator());
    AddRow(gatedCard, MakeRow(autonomousDesktopCheck,
        "Let Revia operate on her own",
        "Unprompted desktop work. Still refused while you are actively working.",
        "Autonomy", "chipAutonomy", false,
        "A separate permission. Without it she may only do this as part of something "
        "you asked for."));
    gatedLayout->addStretch();
    cards->addWidget(gatedCard, 1, 1);

    layout->addLayout(cards);

    // Whole-row clicking for every permission built above.
    for (QFrame* row : findChildren<QFrame*>())
    {
        if (row->property(RowTargetProperty).isValid()) row->installEventFilter(this);
    }

    // ------------------------------------------------------------ approved + discovery
    auto* body = new QHBoxLayout();
    body->setSpacing(18);

    auto* approvedCard = MakeCard("permCard");
    auto* approvedColumn = qobject_cast<QVBoxLayout*>(approvedCard->layout());
    auto* approvedTitle = new QLabel("Approved applications and controls");
    approvedTitle->setObjectName("cardTitle");
    approvedColumn->addWidget(approvedTitle);
    auto* approvedHelp = new QLabel("Only the controls listed here may be invoked or edited.");
    approvedHelp->setObjectName("cardSubtitle");
    approvedHelp->setWordWrap(true);
    approvedColumn->addWidget(approvedHelp);
    approvedColumn->addSpacing(10);
    approvedTree = new QTreeWidget(this);
    approvedTree->setHeaderLabels({"Permission", "Scope"});
    approvedTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    approvedTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    approvedTree->setAlternatingRowColors(true);
    approvedTree->setSelectionMode(QAbstractItemView::SingleSelection);
    approvedTree->setRootIsDecorated(false);
    approvedTree->setIndentation(16);
    approvedTree->setMinimumHeight(240);
    approvedColumn->addWidget(approvedTree, 1);
    auto* approvedButtons = new QHBoxLayout();
    approvedButtons->setSpacing(8);
    auto* addApplicationButton = new QPushButton("Add application", this);
    addApplicationButton->setObjectName("primaryButton");
    auto* removeButton = new QPushButton("Remove selected", this);
    removeButton->setObjectName("destructiveButton");
    approvedButtons->addWidget(addApplicationButton);
    approvedButtons->addWidget(removeButton);
    approvedButtons->addStretch();
    approvedColumn->addSpacing(10);
    approvedColumn->addLayout(approvedButtons);
    body->addWidget(approvedCard, 1);

    auto* discoveryCard = MakeCard("permCard");
    auto* discoveryColumn = qobject_cast<QVBoxLayout*>(discoveryCard->layout());
    auto* discoveryTitle = new QLabel("Foreground control discovery");
    discoveryTitle->setObjectName("cardTitle");
    discoveryColumn->addWidget(discoveryTitle);
    discoveryLabel = new QLabel(
        "Minimize Revia and inspect the application underneath. Discovery changes no permission.",
        this);
    discoveryLabel->setWordWrap(true);
    discoveryLabel->setObjectName("cardSubtitle");
    discoveryColumn->addWidget(discoveryLabel);
    discoveryColumn->addSpacing(10);
    discoveredTable = new QTableWidget(0, 3, this);
    discoveredTable->setHorizontalHeaderLabels({"Control", "Automation ID", "Pattern"});
    discoveredTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    discoveredTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    discoveredTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    discoveredTable->verticalHeader()->setVisible(false);
    discoveredTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    discoveredTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    discoveredTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    discoveredTable->setAlternatingRowColors(true);
    discoveredTable->setShowGrid(false);
    discoveredTable->setMinimumHeight(240);
    discoveryColumn->addWidget(discoveredTable, 1);
    auto* discoveryButtons = new QHBoxLayout();
    discoveryButtons->setSpacing(8);
    auto* discoverButton = new QPushButton("Inspect foreground app", this);
    discoverButton->setObjectName("primaryButton");
    approveDiscoveredButton = new QPushButton("Approve selected controls", this);
    approveDiscoveredButton->setEnabled(false);
    discoveryButtons->addWidget(discoverButton);
    discoveryButtons->addWidget(approveDiscoveredButton);
    discoveryButtons->addStretch();
    discoveryColumn->addSpacing(10);
    discoveryColumn->addLayout(discoveryButtons);
    body->addWidget(discoveryCard, 1);
    layout->addLayout(body, 1);

    statusLabel = new QLabel(
        "Permissions persist in RuntimeData/Capabilities/capabilities.json.", this);
    statusLabel->setObjectName("secondaryText");
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    connect(internetCheck, &QCheckBox::toggled, this, [this]() { ApplyInternetSettings(); });
    connect(automaticLookupCheck, &QCheckBox::toggled, this, [this]() { ApplyInternetSettings(); });
    connect(visibleBrowserCheck, &QCheckBox::toggled,
        this, [this]() { ApplyBrowserSettings(); });
    connect(autonomousResearchCheck, &QCheckBox::toggled,
        this, [this]() { ApplyBrowserSettings(); });
    connect(cameraCheck, &QCheckBox::toggled, this, [this]() { ApplyCameraSettings(); });
    connect(autonomousCameraCheck, &QCheckBox::toggled,
        this, [this]() { ApplyCameraSettings(); });
    connect(pointerCheck, &QCheckBox::toggled,
        this, [this]() { ApplyDesktopControlSettings(); });
    connect(keyboardCheck, &QCheckBox::toggled,
        this, [this]() { ApplyDesktopControlSettings(); });
    connect(launchCheck, &QCheckBox::toggled,
        this, [this]() { ApplyDesktopControlSettings(); });
    connect(rawCoordinateCheck, &QCheckBox::toggled,
        this, [this]() { ApplyDesktopControlSettings(); });
    connect(visualTargetCheck, &QCheckBox::toggled,
        this, [this]() { ApplyDesktopControlSettings(); });
    connect(wholeDesktopCheck, &QCheckBox::toggled,
        this, [this]() { ApplyDesktopControlSettings(); });
    connect(commandSurfaceCheck, &QCheckBox::toggled,
        this, [this]() { ApplyDesktopControlSettings(); });
    connect(autonomousDesktopCheck, &QCheckBox::toggled,
        this, [this]() { ApplyDesktopControlSettings(); });
    connect(desktopStopButton, &QPushButton::clicked, this, [this]() { ToggleDesktopStop(); });
    connect(addApplicationButton, &QPushButton::clicked, this,
        [this]() { AddApplicationManually(); });
    connect(removeButton, &QPushButton::clicked, this,
        [this]() { RemoveSelectedPermission(); });
    connect(discoverButton, &QPushButton::clicked, this, [this]()
    {
        if (requestDiscovery) requestDiscovery();
    });
    connect(approveDiscoveredButton, &QPushButton::clicked, this,
        [this]() { ApproveSelectedDiscoveredControls(); });
    Refresh();
}

void CapabilityPanel::Refresh()
{
    refreshing = true;
    const revia::actions::CapabilitySettings settings = session.Capabilities();
    approvedTree->clear();
    for (const std::string& application : settings.approvedApplications)
    {
        auto* applicationItem = new QTreeWidgetItem(approvedTree);
        applicationItem->setText(0, QString::fromStdString(application));
        applicationItem->setText(1, "Application");
        applicationItem->setData(0, Qt::UserRole, QString::fromStdString(application));
        const auto controls = std::find_if(
            settings.approvedControls.begin(), settings.approvedControls.end(),
            [&](const auto& entry)
            {
                return QString::fromStdString(entry.first).compare(
                    QString::fromStdString(application), Qt::CaseInsensitive) == 0;
            });
        if (controls != settings.approvedControls.end())
        {
            for (const std::string& control : controls->second)
            {
                auto* controlItem = new QTreeWidgetItem(applicationItem);
                controlItem->setText(0, QString::fromStdString(control));
                controlItem->setText(1, control == "*" ? "All mutable controls" : "Control");
                controlItem->setData(0, Qt::UserRole, QString::fromStdString(application));
                controlItem->setData(1, Qt::UserRole, QString::fromStdString(control));
            }
        }
        applicationItem->setExpanded(true);
    }
    internetCheck->setChecked(settings.internet.enabled);
    taskApprovalMode->setCurrentIndex(taskApprovalMode->findData(static_cast<int>(settings.mode)));
    automaticLookupCheck->setChecked(settings.internet.automaticLookup);
    visibleBrowserCheck->setChecked(settings.internet.visibleBrowser);
    autonomousResearchCheck->setChecked(settings.internet.autonomousResearch);
    automaticLookupCheck->setEnabled(settings.internet.enabled);
    visibleBrowserCheck->setEnabled(settings.internet.enabled);
    autonomousResearchCheck->setEnabled(
        settings.internet.enabled && settings.internet.visibleBrowser);
    cameraCheck->setChecked(settings.camera.enabled);
    autonomousCameraCheck->setChecked(settings.camera.autonomousCapture);
    autonomousCameraCheck->setEnabled(settings.camera.enabled);
    const auto& desktop = settings.desktopControl;
    pointerCheck->setChecked(desktop.pointer);
    keyboardCheck->setChecked(desktop.keyboard);
    launchCheck->setChecked(desktop.applicationLaunch);
    rawCoordinateCheck->setChecked(desktop.rawCoordinates);
    rawCoordinateCheck->setEnabled(desktop.pointer);
    visualTargetCheck->setChecked(desktop.visualTargeting);
    visualTargetCheck->setEnabled(desktop.pointer);
    wholeDesktopCheck->setChecked(desktop.scope ==
        revia::actions::CapabilitySettings::DesktopControl::InputScope::WholeDesktop);
    wholeDesktopCheck->setEnabled(desktop.pointer && desktop.rawCoordinates);
    commandSurfaceCheck->setChecked(desktop.allowCommandSurfaces);
    commandSurfaceCheck->setEnabled(desktop.pointer || desktop.keyboard);
    autonomousDesktopCheck->setChecked(desktop.autonomous);
    autonomousDesktopCheck->setEnabled(desktop.AnyEnabled());
    // Kept meaningful even with every capability off: the stop has to stay reachable
    // while it is latched, or the only way back would be editing JSON.
    desktopStopButton->setText(session.DesktopControlStopped()
        ? "Resume desktop control" : "Stop desktop control");
    refreshing = false;
}

void CapabilityPanel::ApplyCameraSettings()
{
    if (refreshing) return;
    const auto previous = session.Capabilities().camera;
    // Confirmed on the way up only, and worded so the answer is informed rather than
    // reflexive. A camera toggle that flips silently is the one permission users would
    // most reasonably expect to be asked about.
    if (!previous.enabled && cameraCheck->isChecked())
    {
        const bool approved = QMessageBox::question(
            this,
            "Allow camera access",
            "Revia will be able to take a single still frame from your camera and describe "
            "what it contains. The camera is opened for that frame and closed again "
            "immediately, so the hardware light is on only while a frame is being taken. "
            "Frames are written to RuntimeData/Camera on this computer and are never "
            "uploaded. Seeing something does not let her act on it: anything she does as a "
            "result still goes through the ordinary permission and confirmation path. "
            "Enable camera access?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            Refresh();
            return;
        }
    }
    if (!previous.autonomousCapture && autonomousCameraCheck->isChecked())
    {
        const bool approved = QMessageBox::question(
            this,
            "Let Revia decide when to look",
            "This is a separate permission from answering a question about what she can see. "
            "It allows Revia to take a frame without you asking, subject to the same rate "
            "limit and the same audit trail. Consenting to answer \"what am I holding?\" is "
            "not the same as consenting to be watched, which is why this is asked "
            "separately. Allow it?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            Refresh();
            return;
        }
    }
    autonomousCameraCheck->setEnabled(cameraCheck->isChecked());
    const auto result = session.SetCameraAccess(
        cameraCheck->isChecked(), autonomousCameraCheck->isChecked());
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::ApplyDesktopControlSettings()
{
    if (refreshing) return;
    const auto previous = session.Capabilities().desktopControl;
    // Confirmed on the way up only. This is the permission that lets Revia act as the
    // person at the keyboard, so the question is asked once and answered informed.
    if (!previous.AnyEnabled() &&
        (pointerCheck->isChecked() || keyboardCheck->isChecked() ||
            launchCheck->isChecked()))
    {
        const bool approved = QMessageBox::question(
            this,
            "Allow pointer and keyboard control",
            "Revia will be able to move the pointer, click, and type on this computer. "
            "Input is confined to windows belonging to the applications approved below, "
            "and she refuses to act when something else has focus, but inside those "
            "windows it is the same as you doing it. Every action is rate limited and "
            "written to the action audit log; typed text is recorded only by length. "
            "Hold ctrl+alt+shift at any time to stop it. Enable desktop control?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            Refresh();
            return;
        }
    }
    if (!previous.autonomous && autonomousDesktopCheck->isChecked())
    {
        const bool approved = QMessageBox::question(
            this,
            "Let Revia operate the desktop on her own",
            "This is a separate permission from doing desktop work you asked for. It "
            "allows Revia to move the pointer, type, and start approved applications "
            "without a request, subject to the same limits and the same audit trail. "
            "Allow it?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            Refresh();
            return;
        }
    }
    using InputScope =
        revia::actions::CapabilitySettings::DesktopControl::InputScope;
    if (previous.scope != InputScope::WholeDesktop && wholeDesktopCheck->isChecked())
    {
        const bool approved = QMessageBox::question(
            this,
            "Give Revia the whole desktop",
            "Until now her input has been confined to windows belonging to the "
            "applications you approved. This removes that confinement: the pointer can "
            "go anywhere on your screens and the keyboard goes to whatever has focus, "
            "exactly as it does for you.\n\n"
            "That is the point of it -- a general skill cannot be learned inside one "
            "application -- but it is worth being clear that confinement is what is "
            "being given up. What remains is this permission, the refusal to touch "
            "terminals and script hosts, the per-minute input budget, the audit log, and "
            "the ctrl+alt+shift stop. Those raise the effort; none of them is a proof "
            "that she cannot reach something you would not have chosen.\n\n"
            "Give her the whole desktop?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            Refresh();
            return;
        }
    }
    if (!previous.allowCommandSurfaces && commandSurfaceCheck->isChecked())
    {
        const bool approved = QMessageBox::question(
            this,
            "Allow terminals and script hosts",
            "Command interpreters, script hosts, and the chords that summon them are "
            "refused by default, because text typed into one of them is model text "
            "running as a command however it arrived. Allowing this makes that possible "
            "on purpose. Allow it?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            Refresh();
            return;
        }
    }
    const auto result = session.SetDesktopControl(
        pointerCheck->isChecked(),
        keyboardCheck->isChecked(),
        launchCheck->isChecked(),
        rawCoordinateCheck->isChecked(),
        visualTargetCheck->isChecked(),
        autonomousDesktopCheck->isChecked(),
        wholeDesktopCheck->isChecked()
            ? InputScope::WholeDesktop : InputScope::ApprovedApplications,
        commandSurfaceCheck->isChecked());
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::ToggleDesktopStop()
{
    if (session.DesktopControlStopped())
    {
        const auto result = session.ResumeDesktopControl();
        SetStatus(QString::fromStdString(result.message), !result.succeeded);
    }
    else
    {
        session.StopDesktopControl("stopped from the Permissions tab");
        SetStatus("Desktop control is stopped. Nothing further will be typed or clicked.");
    }
    Refresh();
}

void CapabilityPanel::ShowDiscovery(
    const revia::actions::windows::ApplicationControlInventory& inventory)
{
    discoveredTable->setRowCount(0);
    discoveredApplication.clear();
    if (!inventory.succeeded)
    {
        discoveryLabel->setText(QString::fromStdString(inventory.reason));
        approveDiscoveredButton->setEnabled(false);
        SetStatus(QString::fromStdString(inventory.reason), true);
        return;
    }
    discoveredApplication = inventory.application;
    discoveryLabel->setText(
        QString::fromStdString(inventory.application + " — " + inventory.windowTitle +
            ". Select only controls Revia should be allowed to change."));
    for (const auto& control : inventory.controls)
    {
        const int row = discoveredTable->rowCount();
        discoveredTable->insertRow(row);
        auto* name = new QTableWidgetItem(QString::fromStdString(
            control.name.empty() ? control.permissionKey : control.name));
        name->setData(Qt::UserRole, QString::fromStdString(control.permissionKey));
        discoveredTable->setItem(row, 0, name);
        discoveredTable->setItem(
            row, 1, new QTableWidgetItem(QString::fromStdString(control.automationId)));
        const QString pattern = control.supportsInvoke && control.supportsValue
            ? "Invoke + Value"
            : control.supportsInvoke ? "Invoke" : "Value";
        discoveredTable->setItem(row, 2, new QTableWidgetItem(pattern));
    }
    approveDiscoveredButton->setEnabled(!inventory.controls.empty());
    SetStatus(QString::fromStdString(inventory.reason));
}

void CapabilityPanel::SetStatus(const QString& text, const bool error)
{
    statusLabel->setText(text);
    statusLabel->setStyleSheet(error ? "color: #ff8da1;" : "color: #85d7c8;");
}

void CapabilityPanel::AddApplicationManually()
{
    bool accepted = false;
    const QString executable = QInputDialog::getText(
        this, "Approve application", "Executable name (for example, notepad.exe)",
        QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || executable.isEmpty()) return;
    const auto result = session.AddApprovedApplication(executable.toStdString());
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::ApproveSelectedDiscoveredControls()
{
    if (discoveredApplication.empty()) return;
    revia::runtime::CapabilityUpdateResult result =
        session.AddApprovedApplication(discoveredApplication);
    if (!result.succeeded)
    {
        SetStatus(QString::fromStdString(result.message), true);
        return;
    }
    const QModelIndexList rows = discoveredTable->selectionModel()->selectedRows();
    for (const QModelIndex& index : rows)
    {
        const QString key = discoveredTable->item(index.row(), 0)->data(Qt::UserRole).toString();
        result = session.AddApprovedControl(discoveredApplication, key.toStdString());
        if (!result.succeeded) break;
    }
    SetStatus(QString::fromStdString(
        rows.empty()
            ? "Application approved for inspection only; no mutable controls were selected."
            : result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::RemoveSelectedPermission()
{
    QTreeWidgetItem* item = approvedTree->currentItem();
    if (item == nullptr) return;
    const QString application = item->data(0, Qt::UserRole).toString();
    const QString control = item->data(1, Qt::UserRole).toString();
    const QString description = control.isEmpty()
        ? QStringLiteral("all permissions for %1").arg(application)
        : QStringLiteral("control '%1' from %2").arg(control, application);
    if (QMessageBox::question(
            this, "Remove permission", "Remove " + description + "?") != QMessageBox::Yes)
    {
        return;
    }
    const auto result = control.isEmpty()
        ? session.RemoveApprovedApplication(application.toStdString())
        : session.RemoveApprovedControl(application.toStdString(), control.toStdString());
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::ApplyInternetSettings()
{
    if (refreshing) return;
    const bool wasEnabled = session.Capabilities().internet.enabled;
    if (!wasEnabled && internetCheck->isChecked())
    {
        const bool approved = QMessageBox::question(
            this,
            "Enable internet lookup",
            "Knowledge questions may be sent to the configured DuckDuckGo and Wikipedia "
            "HTTPS endpoints or, when selected below, to Revia's dedicated visible browser. "
            "Every lookup is read-only, bounded, rate limited, and audited. Enable this "
            "capability?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            refreshing = true;
            internetCheck->setChecked(false);
            refreshing = false;
            return;
        }
    }
    automaticLookupCheck->setEnabled(internetCheck->isChecked());
    visibleBrowserCheck->setEnabled(internetCheck->isChecked());
    autonomousResearchCheck->setEnabled(
        internetCheck->isChecked() && visibleBrowserCheck->isChecked());
    const auto result = session.SetInternetAccess(
        internetCheck->isChecked(), automaticLookupCheck->isChecked());
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::ApplyBrowserSettings()
{
    if (refreshing) return;
    const auto previous = session.Capabilities().internet;
    if (!previous.visibleBrowser && visibleBrowserCheck->isChecked())
    {
        const bool approved = QMessageBox::question(
            this,
            "Enable visible browsing",
            "Revia will open a separate, visible Edge or Chrome window using a dedicated "
            "RuntimeData browser profile. She may search public HTTPS pages and read bounded "
            "page text, but cannot use your personal cookies, downloads, uploads, passwords, "
            "payments, localhost, or private-network pages. Queries, pages, results, timing, "
            "and failures remain visible and audited. Enable it?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            Refresh();
            return;
        }
    }
    if (!previous.autonomousResearch && autonomousResearchCheck->isChecked())
    {
        const bool approved = QMessageBox::question(
            this,
            "Allow autonomous research",
            "This allows Revia to choose a read-only web query without a new message from "
            "you when recent conversation or a meaningful emotion creates a concrete topic. "
            "A timer alone cannot create a topic, and attention, cooldown, deduplication, "
            "hourly, browser, and network limits still apply. Allow this separate permission?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
        if (!approved)
        {
            Refresh();
            return;
        }
    }

    const auto result = session.SetInternetBrowser(
        visibleBrowserCheck->isChecked(),
        autonomousResearchCheck->isChecked());
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}
