#include "capabilityPanel.h"
#include "settingsCatalog.h"
#include "toggleSwitch.h"

#include "Browser/browserTypes.h"
#include "Process/processTypes.h"

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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QStyle>
#include <QSvgWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <filesystem>
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

void AddCardHeader(QFrame* card, const QString& iconPath, const QString& stroke, const QString& iconObjectName, const QString& title,
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

QLabel* MakeBadge(const QString& text, const QString& kind)
{
    auto* badge = new QLabel(text);
    badge->setObjectName("settingBadge");
    badge->setProperty("kind", kind);
    return badge;
}

void SetTone(QLabel* label, const QString& text, const bool error)
{
    label->setText(text);
    label->setProperty("tone", error ? "error" : "success");
    label->style()->unpolish(label);
    label->style()->polish(label);
}

const QString ChangeBack = QStringLiteral("\n\nYou can change this back here at any time.");
}

bool CapabilityPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Wheel && watched == taskApprovalMode && !taskApprovalMode->hasFocus())
    {
        // A notch over the closed list picks the neighbouring mode and applies it at once; one down
        // from the default is "Free". Ignored, the wheel scrolls the page the panel sits in instead.
        event->ignore();
        return true;
    }
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

QString CapabilityPanel::CompanionName() const
{
    return QString::fromStdString(session.DisplayName());
}

QString CapabilityPanel::Warning(const ToggleSwitch* toggle) const
{
    const auto* descriptor = revia::desktop::FindSetting(toggle->objectName().toStdString());
    return descriptor != nullptr ? revia::desktop::WithCompanionName(descriptor->warning, CompanionName()) : QString();
}

// One permission: the catalog's label and one-line help, risk badges, an optional second help
// line, and the switch on the right. The switch must already carry its objectName.
QFrame* CapabilityPanel::MakeRow(ToggleSwitch* toggle, const QString& tip, const bool nested, const QString& riskText)
{
    using revia::desktop::SettingRisk;
    const auto* descriptor = revia::desktop::FindSetting(toggle->objectName().toStdString());
    const QString name = CompanionName();
    const QString label = descriptor != nullptr ? revia::desktop::WithCompanionName(descriptor->label, name) : toggle->objectName();
    const QString help = descriptor != nullptr ? revia::desktop::WithCompanionName(descriptor->description, name) : QString();
    const QString detail = revia::desktop::WithCompanionName(tip, name);

    auto* row = new QFrame();
    row->setObjectName(nested ? "permRowNested" : "permRow");
    row->setProperty(RowTargetProperty, QVariant::fromValue<QObject*>(toggle));
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(nested ? 32 : 16, 12, 16, 12);
    layout->setSpacing(16);

    auto* text = new QVBoxLayout();
    text->setSpacing(4);
    auto* labelWidget = new QLabel(label);
    labelWidget->setObjectName(nested ? "permLabelNested" : "permLabel");
    labelWidget->setWordWrap(true);
    text->addWidget(labelWidget);
    if (!help.isEmpty())
    {
        auto* helpLabel = new QLabel(help);
        helpLabel->setObjectName("permHelp");
        helpLabel->setWordWrap(true);
        text->addWidget(helpLabel);
    }
    if (!detail.isEmpty())
    {
        // What used to hide behind an "i" tooltip is now a visible second line.
        auto* detailLabel = new QLabel(detail);
        detailLabel->setObjectName("permRowHelp");
        detailLabel->setWordWrap(true);
        text->addWidget(detailLabel);
    }
    // The same badges, in the same place, as a Settings row, so risk reads alike in both.
    QStringList badges;
    if (descriptor != nullptr)
    {
        if (descriptor->risk == SettingRisk::Privacy)
            badges.push_back(QStringLiteral("privacy:Privacy"));
        if (descriptor->risk == SettingRisk::Dangerous)
            badges.push_back(QStringLiteral("danger:") + (riskText.isEmpty() ? QStringLiteral("Risky") : riskText));
        if (descriptor->autonomy)
            badges.push_back(QStringLiteral("autonomy:Autonomy"));
    }
    if (!badges.isEmpty())
    {
        auto* badgeRow = new QHBoxLayout();
        badgeRow->setContentsMargins(0, 2, 0, 0);
        badgeRow->setSpacing(6);
        for (QString& badge : badges)
        {
            const qsizetype colon = badge.indexOf(QLatin1Char(':'));
            badgeRow->addWidget(MakeBadge(badge.mid(colon + 1), badge.left(colon)));
            badge = badge.mid(colon + 1);
        }
        badgeRow->addStretch(1);
        text->addLayout(badgeRow);
    }
    layout->addLayout(text, 1);
    layout->addWidget(toggle, 0, Qt::AlignVCenter);

    QString description = help;
    if (!detail.isEmpty())
        description += (description.isEmpty() ? QString() : QStringLiteral(" ")) + detail;
    if (!badges.isEmpty())
        description += QStringLiteral(" (") + badges.join(QStringLiteral(", ")) + QLatin1Char(')');
    row->setAccessibleName(label);
    row->setAccessibleDescription(description);
    toggle->setAccessibleName(label);
    toggle->setAccessibleDescription(description);
    return row;
}

CapabilityPanel::CapabilityPanel(revia::runtime::ReviaSession& inputSession, DiscoveryRequest inputDiscoveryRequest, QWidget* parent)
    : QWidget(parent),
      session(inputSession),
      requestDiscovery(std::move(inputDiscoveryRequest))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(16);

    auto* title = new QLabel("Application, command and internet permissions", this);
    title->setObjectName("sectionTitle");
    layout->addWidget(title);

    auto* intro = new QFrame(this);
    intro->setObjectName("introBar");
    auto* introLayout = new QVBoxLayout(intro);
    introLayout->setContentsMargins(14, 11, 14, 11);
    auto* explanation = new QLabel(
        QStringLiteral("Applications begin with read-only inspection permission and no mutable controls. "
                       "Discover a foreground window, then approve only the controls %1 may invoke or edit. "
                       "Internet access is a separate read-only, rate-limited capability.").arg(CompanionName()), intro);
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
    taskApprovalMode->setAccessibleName("Requested tasks");
    using ExecutionMode = revia::actions::ExecutionMode;
    taskApprovalMode->addItem("Approve once at the start", static_cast<int>(ExecutionMode::Supervised));
    taskApprovalMode->addItem("Free — use granted permissions without asking", static_cast<int>(ExecutionMode::OwnerFullAccess));
    taskApprovalMode->addItem("Approved scope only", static_cast<int>(ExecutionMode::ApprovedScope));
    taskApprovalMode->addItem("Actions disabled", static_cast<int>(ExecutionMode::Disabled));
    // Changed only once clicked or tabbed into; see eventFilter.
    taskApprovalMode->setFocusPolicy(Qt::StrongFocus);
    taskApprovalMode->installEventFilter(this);
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

    // ------------------------------------------------------------ internet group
    // Built inside its own container so the Settings dialog can show it in the Internet section;
    // until then it sits here, and the panel works on its own.
    internetGroup = new QWidget(this);
    auto* internetLayout = new QVBoxLayout(internetGroup);
    internetLayout->setContentsMargins(0, 0, 0, 0);
    internetLayout->setSpacing(18);
    internetLayout->addWidget(CreateInternetCard());
    internetLayout->addWidget(CreateInteractiveBrowserCard());
    internetStatusLabel = new QLabel(internetGroup);
    internetStatusLabel->setObjectName("settingStatus");
    internetStatusLabel->setWordWrap(true);
    internetStatusLabel->hide();
    internetLayout->addWidget(internetStatusLabel);
    layout->addWidget(internetGroup);

    auto* cards = new QGridLayout();
    cards->setObjectName("permissionCards");
    cards->setHorizontalSpacing(18);
    cards->setVerticalSpacing(18);
    cards->setColumnStretch(0, 1);
    cards->setColumnStretch(1, 1);

    // ------------------------------------------------------------ camera
    auto* cameraCard = MakeCard("permCard");
    AddCardHeader(cameraCard,
        "<path d='M3 8.5A2.5 2.5 0 0 1 5.5 6h2L9 4h6l1.5 2h2A2.5 2.5 0 0 1 21 8.5v8A2.5 "
        "2.5 0 0 1 18.5 19h-13A2.5 2.5 0 0 1 3 16.5z'/><circle cx='12' cy='12.5' r='3.4'/>",
        "#55E8F2", "iconWell",
        "Camera",
        "Opened for a single frame, then closed again immediately.");
    cameraCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    cameraCheck->setObjectName("cameraCheck");
    autonomousCameraCheck = new ToggleSwitch(ToggleSwitch::Accent::Violet, this);
    autonomousCameraCheck->setObjectName("autonomousCameraCheck");
    AddRow(cameraCard, MakeRow(cameraCheck,
        "{name} may take a single still frame when you ask about something in view. The camera is opened for that "
        "frame and closed again immediately."));
    AddRow(cameraCard, MakeRow(autonomousCameraCheck,
        "A separate permission. Without it the camera is used only when you ask.", true));
    qobject_cast<QVBoxLayout*>(cameraCard->layout())->addStretch();
    cards->addWidget(cameraCard, 0, 0);

    // ------------------------------------------------------------ pointer and keyboard
    auto* desktopCard = MakeCard("permCard");
    AddCardHeader(desktopCard,
        "<path d='M5 3.5 18.5 11l-5.8 1.6L10 19z'/>",
        "#55E8F2", "iconWell",
        "Pointer & Keyboard",
        "Synthesized input is indistinguishable from you typing. Confined to approved "
        "applications unless that confinement is lifted.");
    pointerCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    pointerCheck->setObjectName("pointerCheck");
    keyboardCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    keyboardCheck->setObjectName("keyboardCheck");
    launchCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    launchCheck->setObjectName("launchCheck");
    rawCoordinateCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    rawCoordinateCheck->setObjectName("rawCoordinateCheck");
    visualTargetCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    visualTargetCheck->setObjectName("visualTargetCheck");
    AddRow(desktopCard, MakeRow(pointerCheck));
    AddRow(desktopCard, MakeRow(keyboardCheck));
    AddRow(desktopCard, MakeRow(launchCheck));
    AddRow(desktopCard, MakeRow(visualTargetCheck,
        "The target must be something {name} just looked at, in the window looked at, still in front and still the "
        "same size. Anything else is refused and {name} looks again.", true));
    AddRow(desktopCard, MakeRow(rawCoordinateCheck,
        "Wider than the setting above: a point with nothing behind it, rather than a thing {name} was looking at.",
        true));

    auto* desktopLayout = qobject_cast<QVBoxLayout*>(desktopCard->layout());
    desktopLayout->addSpacing(6);
    auto* stopBar = new QFrame();
    stopBar->setObjectName("stopBar");
    auto* stopLayout = new QHBoxLayout(stopBar);
    stopLayout->setContentsMargins(11, 9, 11, 9);
    stopLayout->setSpacing(10);
    auto* stopState = new QLabel("Hold Ctrl+Alt+Shift to halt input immediately.");
    stopState->setObjectName("stopState");
    stopState->setWordWrap(true);
    stopLayout->addWidget(stopState, 1);
    // Named apart from the composer's stopButton, which stops a reply rather than desktop control.
    desktopStopButton = new QPushButton("Stop desktop control", this);
    desktopStopButton->setObjectName("desktopStopButton");
    stopLayout->addWidget(desktopStopButton, 0);
    desktopLayout->addWidget(stopBar);
    desktopLayout->addStretch();
    cards->addWidget(desktopCard, 0, 1);

    // ------------------------------------------------------------ gated
    auto* gatedCard = MakeCard("gatedCard");
    AddCardHeader(gatedCard,
        "<path d='M12 3 4 6.2v5.3c0 4.6 3.2 8.6 8 9.5 4.8-.9 8-4.9 8-9.5V6.2z'/>"
        "<path d='M12 9v4'/><path d='M12 16h.01'/>",
        "#F3A446", "iconWellGated",
        "Gated Controls",
        "Each one removes a boundary the other permissions rely on. Off by default.");
    auto* gatedNotice = new QLabel(QStringLiteral(
        "These extend %1 beyond approved applications. Policy, confirmation and the "
        "audit log still apply to every action, but the scope they are checked "
        "against grows.").arg(CompanionName()));
    gatedNotice->setObjectName("gatedNotice");
    gatedNotice->setWordWrap(true);
    auto* gatedLayout = qobject_cast<QVBoxLayout*>(gatedCard->layout());
    gatedLayout->addWidget(gatedNotice);
    gatedLayout->addSpacing(4);

    wholeDesktopCheck = new ToggleSwitch(ToggleSwitch::Accent::Amber, this);
    wholeDesktopCheck->setObjectName("wholeDesktopCheck");
    commandSurfaceCheck = new ToggleSwitch(ToggleSwitch::Accent::Red, this);
    commandSurfaceCheck->setObjectName("commandSurfaceCheck");
    autonomousDesktopCheck = new ToggleSwitch(ToggleSwitch::Accent::Violet, this);
    autonomousDesktopCheck->setObjectName("autonomousDesktopCheck");
    AddRow(gatedCard, MakeRow(wholeDesktopCheck,
        "The pointer goes anywhere and the keyboard goes to whatever has focus, the way it does for you.", false,
        "Removes confinement"));
    AddRow(gatedCard, MakeSeparator());
    AddRow(gatedCard, MakeRow(commandSurfaceCheck,
        "Off by default. Typing into a command interpreter is model text reaching a shell, whichever window it "
        "arrived through.", false, "Command surface"));
    AddRow(gatedCard, MakeSeparator());
    AddRow(gatedCard, MakeRow(autonomousDesktopCheck,
        "A separate permission. Without it desktop work happens only as part of something you asked for."));
    gatedLayout->addStretch();
    cards->addWidget(gatedCard, 1, 0);

    // ------------------------------------------------------------ commands
    auto* processCard = MakeCard("permCard");
    AddCardHeader(processCard, "<rect x='3' y='4' width='18' height='16' rx='2'/><path d='m7 9 3 3-3 3M13 15h4'/>", "#55E8F2", "iconWell",
        "Commands", "Run approved programs with bounded time and output.");
    processCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    processCheck->setObjectName("processEnabled");
    taskProcessCheck = new ToggleSwitch(ToggleSwitch::Accent::Violet, this);
    taskProcessCheck->setObjectName("processTaskExecution");
    commandInterpreterCheck = new ToggleSwitch(ToggleSwitch::Accent::Amber, this);
    commandInterpreterCheck->setObjectName("processCommandInterpreters");
    AddRow(processCard, MakeRow(processCheck,
        "Use an approved working directory. Programs keep their normal Windows account access."));
    AddRow(processCard, MakeRow(commandInterpreterCheck,
        "Shells and script hosts must also be listed below. Task approval and execution limits still apply.", false,
        "Command surface"));
    AddRow(processCard, MakeRow(taskProcessCheck,
        "Delegated work uses the same approved executables, working directories and execution limits."));
    auto* processLayout = qobject_cast<QVBoxLayout*>(processCard->layout());
    processLayout->addSpacing(8);
    auto* executableLabel = new QLabel("Approved executable paths — one absolute path per line");
    executableLabel->setObjectName("permLabel");
    executableLabel->setWordWrap(true);
    processLayout->addWidget(executableLabel);
    processLayout->addSpacing(6);
    approvedExecutablesEdit = new QPlainTextEdit(this);
    approvedExecutablesEdit->setObjectName("processApprovedExecutables");
    approvedExecutablesEdit->setAccessibleName("Approved executable paths");
    approvedExecutablesEdit->setPlaceholderText("C:\\Tools\\program.exe");
    approvedExecutablesEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    approvedExecutablesEdit->setMinimumHeight(96);
    approvedExecutablesEdit->setMaximumHeight(160);
    executableLabel->setBuddy(approvedExecutablesEdit);
    processLayout->addWidget(approvedExecutablesEdit);
    processLayout->addSpacing(8);
    processStatusLabel = new QLabel(this);
    processStatusLabel->setObjectName("secondaryText");
    processStatusLabel->setWordWrap(true);
    processLayout->addWidget(processStatusLabel);
    processLayout->addSpacing(8);
    processListFooter = CreateListFooter(applyProcessButton, "Apply command permissions", [this]() { ApplyProcessSettings(); },
        [this]()
        {
            processSettingsDirty = false;
            Refresh();
        });
    processLayout->addWidget(processListFooter);
    cards->addWidget(processCard, 1, 1);
    layout->addLayout(cards);

    // Whole-row clicking for every permission built above, the internet group included.
    for (QFrame* row : findChildren<QFrame*>())
    {
        if (row->property(RowTargetProperty).isValid()) row->installEventFilter(this);
    }

    // ------------------------------------------------------------ approved + discovery
    auto* body = new QHBoxLayout();
    body->setObjectName("permissionApplications");
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
    statusLabel->setObjectName("settingStatus");
    statusLabel->setProperty("tone", "info");
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    // Turning on a risky switch asks first; turning anything off never does.
    const auto confirmFirst = [this](ToggleSwitch* toggle, const bool on, const QString& explanation, std::function<void()> apply)
    {
        if (refreshing) return;
        if (on)
            ConfirmTurnOn(toggle, explanation, std::move(apply));
        else
            apply();
    };
    connect(internetCheck, &QCheckBox::toggled, this, [this, confirmFirst](const bool on)
    {
        confirmFirst(internetCheck, on,
            "Knowledge questions may be sent to the configured DuckDuckGo and Wikipedia HTTPS endpoints or, when "
            "selected below, to a dedicated visible browser. Every lookup is read-only, bounded, rate limited and "
            "audited.",
            [this]() { ApplyInternetSettings(); });
    });
    connect(automaticLookupCheck, &QCheckBox::toggled, this, [this]() { ApplyInternetSettings(); });
    connect(visibleBrowserCheck, &QCheckBox::toggled, this, [this, confirmFirst](const bool on)
    {
        confirmFirst(visibleBrowserCheck, on,
            QStringLiteral("%1 will open a separate, visible Edge or Chrome window using a dedicated RuntimeData browser "
                           "profile, to search public HTTPS pages and read bounded page text. Your personal cookies, "
                           "downloads, uploads, passwords, payments, localhost and private-network pages stay out of "
                           "reach. Queries, pages, results, timing and failures remain visible and audited.")
                .arg(CompanionName()),
            [this]() { ApplyBrowserSettings(); });
    });
    connect(autonomousResearchCheck, &QCheckBox::toggled, this, [this, confirmFirst](const bool on)
    {
        confirmFirst(autonomousResearchCheck, on,
            QStringLiteral("This lets %1 choose a read-only web query without a new message from you when recent "
                           "conversation or a meaningful emotion creates a concrete topic. A timer alone cannot create a "
                           "topic, and attention, cooldown, deduplication, hourly, browser and network limits still apply.")
                .arg(CompanionName()),
            [this]() { ApplyBrowserSettings(); });
    });
    connect(cameraCheck, &QCheckBox::toggled, this, [this, confirmFirst](const bool on)
    {
        confirmFirst(cameraCheck, on,
            QStringLiteral("%1 will be able to take a single still frame from your camera and describe what it contains. "
                           "The camera is opened for that frame and closed again immediately, so the hardware light is on "
                           "only while a frame is being taken. Frames are written to RuntimeData/Camera on this computer "
                           "and are never uploaded. Seeing something grants nothing new: anything done as a result still "
                           "goes through the ordinary permission and confirmation path.")
                .arg(CompanionName()),
            [this]() { ApplyCameraSettings(); });
    });
    connect(autonomousCameraCheck, &QCheckBox::toggled, this, [this, confirmFirst](const bool on)
    {
        confirmFirst(autonomousCameraCheck, on,
            QStringLiteral("This is a separate permission from answering a question about what %1 can see. It allows a "
                           "frame to be taken without you asking, subject to the same rate limit and the same audit trail. "
                           "Consenting to answer \"what am I holding?\" is not the same as consenting to be watched, which "
                           "is why this is asked separately.")
                .arg(CompanionName()),
            [this]() { ApplyCameraSettings(); });
    });
    for (ToggleSwitch* toggle : {processCheck, taskProcessCheck, commandInterpreterCheck})
    {
        connect(toggle, &QCheckBox::toggled, this, [this, toggle, confirmFirst](const bool on)
        {
            confirmFirst(toggle, on, Warning(toggle), [this, toggle]()
            {
                const bool value = toggle->isChecked();
                if (toggle == processCheck)
                    CommitProcessFlag([value](revia::process::ProcessSettings& settings) { settings.enabled = value; });
                else if (toggle == taskProcessCheck)
                    CommitProcessFlag([value](revia::process::ProcessSettings& settings) { settings.allowTaskExecution = value; });
                else
                    CommitProcessFlag([value](revia::process::ProcessSettings& settings) { settings.allowCommandInterpreters = value; });
            });
        });
    }
    connect(approvedExecutablesEdit, &QPlainTextEdit::textChanged, this, [this]()
    {
        if (refreshing) return;
        processSettingsDirty = true;
        processListFooter->setVisible(true);
        processStatusLabel->setText("Unsaved list changes. The saved list stays in effect until Apply.");
    });
    for (ToggleSwitch* toggle : {pointerCheck, keyboardCheck, launchCheck, rawCoordinateCheck, visualTargetCheck, wholeDesktopCheck,
             commandSurfaceCheck, autonomousDesktopCheck})
    {
        connect(toggle, &QCheckBox::toggled, this, [this, toggle, confirmFirst](const bool on)
        {
            QString explanation = Warning(toggle);
            const bool hand = toggle == pointerCheck || toggle == keyboardCheck || toggle == launchCheck;
            if (hand && !session.Capabilities().desktopControl.AnyEnabled())
            {
                // The first hand turned on is the moment desktop control as a whole begins.
                explanation = QStringLiteral(
                    "%1 will be able to move the pointer, click and type on this computer. Input is confined to windows "
                    "belonging to the applications approved below, and is refused when something else has focus, but "
                    "inside those windows it is the same as you doing it. Every action is rate limited and written to "
                    "the action audit log; typed text is recorded only by length. Hold Ctrl+Alt+Shift at any time to "
                    "stop it.").arg(CompanionName());
            }
            else if (toggle == autonomousDesktopCheck)
            {
                explanation = QStringLiteral(
                    "This is a separate permission from doing desktop work you asked for. It allows %1 to move the "
                    "pointer, type and start approved applications without a request, subject to the same limits and "
                    "the same audit trail.").arg(CompanionName());
            }
            else if (toggle == wholeDesktopCheck)
            {
                explanation = QStringLiteral(
                    "Until now input has been confined to windows belonging to the applications you approved. This "
                    "removes that confinement: the pointer can go anywhere on your screens and the keyboard goes to "
                    "whatever has focus, exactly as it does for you.\n\n"
                    "That is the point of it, since a general skill cannot be learned inside one application, but "
                    "confinement is what is being given up. What remains is this permission, the refusal to touch "
                    "terminals and script hosts, the per-minute input budget, the audit log and the Ctrl+Alt+Shift "
                    "stop. Those raise the effort; none of them proves that nothing you would not have chosen can be "
                    "reached.");
            }
            else if (toggle == commandSurfaceCheck)
            {
                explanation = QStringLiteral(
                    "Command interpreters, script hosts and the chords that summon them are refused by default, because "
                    "text typed into one of them is model text running as a command however it arrived. Allowing this "
                    "makes that possible on purpose.");
            }
            confirmFirst(toggle, on, explanation, [this]() { ApplyDesktopControlSettings(); });
        });
    }
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

CapabilityPanel::~CapabilityPanel()
{
    // Once detached the group lives in another widget's layout, but it is still this panel's
    // to delete: its switches call back into this object.
    delete internetGroup.data();
}

QWidget* CapabilityPanel::DetachInternetGroup()
{
    if (internetGroup != nullptr && !internetDetached)
    {
        internetDetached = true;
        layout()->removeWidget(internetGroup);
        internetGroup->setParent(nullptr);
    }
    return internetGroup;
}

QFrame* CapabilityPanel::CreateInternetCard()
{
    auto* internetCard = MakeCard("permCard");
    AddCardHeader(internetCard,
        "<circle cx='12' cy='12' r='9'/><path d='M3 12h18M12 3c2.5 2.6 2.5 15.4 0 18M12 3"
        "c-2.5 2.6-2.5 15.4 0 18'/>",
        "#55E8F2", "iconWell", "Internet & Research", "Read-only and rate-limited. Results are grounding, never instructions.");
    internetCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    internetCheck->setObjectName("internetCheck");
    automaticLookupCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    automaticLookupCheck->setObjectName("automaticLookupCheck");
    visibleBrowserCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    visibleBrowserCheck->setObjectName("visibleBrowserCheck");
    autonomousResearchCheck = new ToggleSwitch(ToggleSwitch::Accent::Violet, this);
    autonomousResearchCheck->setObjectName("autonomousResearchCheck");
    AddRow(internetCard, MakeRow(internetCheck));
    AddRow(internetCard, MakeRow(automaticLookupCheck, "Without this, lookups happen only when you ask for one.", true));
    AddRow(internetCard, MakeRow(visibleBrowserCheck, QString(), true));
    AddRow(internetCard, MakeRow(autonomousResearchCheck, QString(), true));
    qobject_cast<QVBoxLayout*>(internetCard->layout())->addStretch();
    return internetCard;
}

QWidget* CapabilityPanel::CreateListFooter(QPushButton*& apply, const QString& applyName, std::function<void()> onApply,
    std::function<void()> onDiscard)
{
    auto* footer = new QWidget(this);
    footer->setObjectName("capabilityListFooter");
    footer->setAttribute(Qt::WA_StyledBackground, true);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(12, 8, 8, 8);
    footerLayout->setSpacing(8);
    auto* unsaved = new QLabel("Unsaved changes", footer);
    unsaved->setObjectName("secondaryText");
    footerLayout->addWidget(unsaved, 1);
    auto* discard = new QPushButton("Discard", footer);
    discard->setAccessibleName(QStringLiteral("Discard %1").arg(applyName.mid(QStringLiteral("Apply ").size())));
    footerLayout->addWidget(discard);
    apply = new QPushButton("Apply", footer);
    apply->setObjectName("primaryButton");
    apply->setAccessibleName(applyName);
    footerLayout->addWidget(apply);
    connect(apply, &QPushButton::clicked, this, std::move(onApply));
    connect(discard, &QPushButton::clicked, this, std::move(onDiscard));
    footer->hide();
    return footer;
}

QFrame* CapabilityPanel::CreateInteractiveBrowserCard()
{
    auto* card = MakeCard("permCard");
    AddCardHeader(card, "<rect x='3' y='4' width='18' height='16' rx='2'/><path d='M3 9h18M7 6.5h.01M10 6.5h.01m-1 6 6 3-3 1-1 3z'/>",
        "#55E8F2", "iconWell", "Interactive Browser", "Navigate pages, click controls and fill forms on sites you approve.");
    interactiveBrowserCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    interactiveBrowserCheck->setObjectName("browserEnabled");
    browserNavigateCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    browserNavigateCheck->setObjectName("browserNavigate");
    browserInteractCheck = new ToggleSwitch(ToggleSwitch::Accent::Amber, this);
    browserInteractCheck->setObjectName("browserInteract");
    taskBrowserCheck = new ToggleSwitch(ToggleSwitch::Accent::Violet, this);
    taskBrowserCheck->setObjectName("browserTaskInteraction");
    browserLoopbackCheck = new ToggleSwitch(ToggleSwitch::Accent::Amber, this);
    browserLoopbackCheck->setObjectName("browserLoopback");
    AddRow(card, MakeRow(interactiveBrowserCheck));
    AddRow(card, MakeRow(browserNavigateCheck, QString(), true));
    AddRow(card, MakeRow(browserInteractCheck, QString(), true));
    AddRow(card, MakeRow(taskBrowserCheck));
    AddRow(card, MakeRow(browserLoopbackCheck, "Localhost and loopback addresses still need an approved origin below."));
    auto* cardLayout = qobject_cast<QVBoxLayout*>(card->layout());
    cardLayout->addSpacing(8);
    auto* originsLabel = new QLabel("Approved site origins — one per line");
    originsLabel->setObjectName("permLabel");
    originsLabel->setWordWrap(true);
    cardLayout->addWidget(originsLabel);
    auto* originsHelp = new QLabel("Use https://host[:port] with no path or trailing slash. Each site needs its own origin.");
    originsHelp->setObjectName("permHelp");
    originsHelp->setWordWrap(true);
    cardLayout->addWidget(originsHelp);
    cardLayout->addSpacing(6);
    approvedOriginsEdit = new QPlainTextEdit(this);
    approvedOriginsEdit->setObjectName("browserApprovedOrigins");
    approvedOriginsEdit->setAccessibleName("Approved browser site origins");
    approvedOriginsEdit->setPlaceholderText("https://example.com\nhttps://app.example.com:8443");
    approvedOriginsEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    approvedOriginsEdit->setMinimumHeight(96);
    approvedOriginsEdit->setMaximumHeight(160);
    originsLabel->setBuddy(approvedOriginsEdit);
    cardLayout->addWidget(approvedOriginsEdit);
    cardLayout->addSpacing(8);
    browserStatusLabel = new QLabel(this);
    browserStatusLabel->setObjectName("secondaryText");
    browserStatusLabel->setWordWrap(true);
    cardLayout->addWidget(browserStatusLabel);
    cardLayout->addSpacing(8);
    browserListFooter = CreateListFooter(applyInteractiveBrowserButton, "Apply interactive browser permissions",
        [this]() { ApplyInteractiveBrowserSettings(); },
        [this]()
        {
            browserSettingsDirty = false;
            Refresh();
        });
    cardLayout->addWidget(browserListFooter);

    const auto commitFlag = [this](ToggleSwitch* toggle)
    {
        const bool value = toggle->isChecked();
        if (toggle == interactiveBrowserCheck)
            CommitBrowserFlag([value](revia::browser::BrowserSettings& settings) { settings.enabled = value; });
        else if (toggle == browserNavigateCheck)
            CommitBrowserFlag([value](revia::browser::BrowserSettings& settings) { settings.navigate = value; });
        else if (toggle == browserInteractCheck)
            CommitBrowserFlag([value](revia::browser::BrowserSettings& settings) { settings.interact = value; });
        else if (toggle == taskBrowserCheck)
            CommitBrowserFlag([value](revia::browser::BrowserSettings& settings) { settings.allowTaskInteraction = value; });
        else
            CommitBrowserFlag([value](revia::browser::BrowserSettings& settings) { settings.allowLoopback = value; });
    };
    for (ToggleSwitch* toggle : {interactiveBrowserCheck, browserNavigateCheck, browserInteractCheck, taskBrowserCheck, browserLoopbackCheck})
    {
        const bool risky = toggle == browserInteractCheck || toggle == taskBrowserCheck || toggle == browserLoopbackCheck;
        connect(toggle, &QCheckBox::toggled, this, [this, toggle, risky, commitFlag](const bool on)
        {
            if (refreshing) return;
            if (on && risky)
                ConfirmTurnOn(toggle, Warning(toggle), [toggle, commitFlag]() { commitFlag(toggle); });
            else
                commitFlag(toggle);
        });
    }
    connect(approvedOriginsEdit, &QPlainTextEdit::textChanged, this, [this]()
    {
        if (refreshing) return;
        browserSettingsDirty = true;
        browserListFooter->setVisible(true);
        browserStatusLabel->setText("Unsaved list changes. The saved list stays in effect until Apply.");
    });
    return card;
}

void CapabilityPanel::ConfirmTurnOn(ToggleSwitch* toggle, const QString& explanation, std::function<void()> apply)
{
    {
        const QSignalBlocker blocker(toggle);
        toggle->setChecked(false);
    }
    const auto* descriptor = revia::desktop::FindSetting(toggle->objectName().toStdString());
    const QString label = descriptor != nullptr ? revia::desktop::WithCompanionName(descriptor->label, CompanionName())
                                                : toggle->accessibleName();
    auto* box = new QMessageBox(this);
    box->setObjectName("settingConfirmDialog");
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(QStringLiteral("Turn on %1?").arg(label));
    box->setTextFormat(Qt::PlainText);
    box->setText((explanation.isEmpty() ? label : explanation) + ChangeBack);
    QPushButton* accept = box->addButton("Turn on", QMessageBox::AcceptRole);
    QPushButton* cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(cancel);
    box->setEscapeButton(cancel);
    const QPointer<QCheckBox> target(toggle);
    connect(box, &QMessageBox::finished, this, [box, accept, target, apply = std::move(apply)](int)
    {
        if (box->clickedButton() != accept || target == nullptr || !target->isEnabled())
            return;
        {
            const QSignalBlocker blocker(target);
            target->setChecked(true);
        }
        apply();
    });
    box->open();
}

void CapabilityPanel::RefreshInteractiveBrowserSettings(const revia::actions::CapabilitySettings& settings)
{
    const auto& browser = settings.browser;
    interactiveBrowserCheck->setChecked(browser.enabled);
    browserNavigateCheck->setChecked(browser.navigate);
    browserInteractCheck->setChecked(browser.interact);
    taskBrowserCheck->setChecked(browser.allowTaskInteraction);
    browserLoopbackCheck->setChecked(browser.allowLoopback);
    for (ToggleSwitch* toggle : {browserNavigateCheck, browserInteractCheck, taskBrowserCheck, browserLoopbackCheck})
        toggle->setEnabled(browser.enabled);
    if (!browserSettingsDirty)
    {
        QStringList origins;
        for (const auto& origin : browser.approvedOrigins)
        {
            origins.push_back(QString::fromStdString(origin));
        }
        const QString savedOrigins = origins.join('\n');
        if (approvedOriginsEdit->toPlainText() != savedOrigins)
        {
            approvedOriginsEdit->setPlainText(savedOrigins);
        }
    }
    QString status = "Interactive browsing is disabled.";
    if (browser.enabled)
    {
        status = browser.approvedOrigins.empty()
                     ? "No site origins are approved. Browser actions cannot run yet."
                     : QString("Interactive browsing enabled for %1 site(s).").arg(static_cast<qulonglong>(browser.approvedOrigins.size()));
        status += browser.navigate ? " Navigation allowed." : " Navigation blocked.";
        status += browser.interact ? " Interaction allowed." : " Interaction blocked.";
        status += browser.allowTaskInteraction ? " Delegated browser use allowed." : " Delegated browser use blocked.";
    }
    if (settings.mode == revia::actions::ExecutionMode::Disabled)
    {
        status += " Requested tasks are disabled.";
    }
    if (browserSettingsDirty)
    {
        status += " The edited list is not applied yet.";
    }
    browserStatusLabel->setText(status);
    browserListFooter->setVisible(browserSettingsDirty);
}

// Apply for the origins list only; the switches beside it have already committed.
void CapabilityPanel::ApplyInteractiveBrowserSettings()
{
    if (refreshing)
    {
        return;
    }
    revia::browser::BrowserSettings settings = session.Capabilities().browser;
    settings.approvedOrigins.clear();
    const QStringList lines = approvedOriginsEdit->toPlainText().split('\n');
    for (const QString& line : lines)
    {
        const QString origin = line.trimmed();
        if (!origin.isEmpty())
        {
            settings.approvedOrigins.push_back(origin.toStdString());
        }
    }
    std::string error;
    if (!revia::browser::ValidateSettings(settings, error))
    {
        browserStatusLabel->setText(QString::fromStdString(error));
        ShowInternetResult(QString::fromStdString(error), true);
        approvedOriginsEdit->setFocus();
        return;
    }
    const auto result = session.SetInteractiveBrowser(settings);
    if (result.succeeded)
    {
        browserSettingsDirty = false;
    }
    Refresh();
    ShowInternetResult(QString::fromStdString(result.message), !result.succeeded);
    if (!result.succeeded)
    {
        browserStatusLabel->setText(QString::fromStdString(result.message));
    }
}

// One browser switch: the saved settings with that one flag changed. A refusal leaves the
// saved value in place and Refresh puts the switch back.
void CapabilityPanel::CommitBrowserFlag(const std::function<void(revia::browser::BrowserSettings&)>& change)
{
    revia::browser::BrowserSettings settings = session.Capabilities().browser;
    change(settings);
    std::string error;
    if (!revia::browser::ValidateSettings(settings, error))
    {
        ShowInternetResult(QString::fromStdString(error), true);
        Refresh();
        return;
    }
    const auto result = session.SetInteractiveBrowser(settings);
    ShowInternetResult(QString::fromStdString(result.message), !result.succeeded);
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
    processCheck->setChecked(settings.process.enabled);
    taskProcessCheck->setChecked(settings.process.allowTaskExecution);
    commandInterpreterCheck->setChecked(settings.process.allowCommandInterpreters);
    if (!processSettingsDirty)
    {
        QStringList executablePaths;
        for (const auto& executable : settings.process.approvedExecutables)
        {
            executablePaths.push_back(QString::fromStdWString(executable.wstring()));
        }
        const QString savedPaths = executablePaths.join('\n');
        if (approvedExecutablesEdit->toPlainText() != savedPaths)
        {
            approvedExecutablesEdit->setPlainText(savedPaths);
        }
    }
    QString processStatus = "Commands are disabled.";
    if (settings.process.enabled)
    {
        processStatus =
            settings.process.approvedExecutables.empty()
                ? "No executable paths are approved. Commands cannot run yet."
                : QString("Commands enabled for %1 approved executable(s). %2")
                      .arg(static_cast<qulonglong>(settings.process.approvedExecutables.size()))
                      .arg(settings.process.allowCommandInterpreters ? "Approved interpreters are allowed." : "Interpreters are blocked.");
        processStatus += settings.process.allowTaskExecution ? " Delegated command use is allowed." : " Delegated command use is blocked.";
    }
    if (settings.mode == revia::actions::ExecutionMode::Disabled)
    {
        processStatus += " Requested tasks are disabled.";
    }
    if (processSettingsDirty)
    {
        processStatus += " The edited list is not applied yet.";
    }
    processStatusLabel->setText(processStatus);
    processListFooter->setVisible(processSettingsDirty);
    RefreshInteractiveBrowserSettings(settings);
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

void CapabilityPanel::ApplyContentWidth(const int width)
{
    if (auto* cards = findChild<QGridLayout*>("permissionCards"))
    {
        const int columns = width >= 900 ? 2 : 1;
        QList<QLayoutItem*> items;
        while (cards->count() > 0)
            items.push_back(cards->takeAt(0));
        for (int index = 0; index < items.size(); ++index)
            cards->addItem(items[index], index / columns, index % columns);
        cards->setColumnStretch(1, columns == 2 ? 1 : 0);
    }
    if (auto* applications = findChild<QBoxLayout*>("permissionApplications"))
        applications->setDirection(width >= 760 ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    approvedTree->setMinimumWidth(0);
    discoveredTable->setMinimumWidth(0);
}

void CapabilityPanel::RevertUnsavedLists()
{
    if (!processSettingsDirty && !browserSettingsDirty)
        return;
    processSettingsDirty = false;
    browserSettingsDirty = false;
    Refresh();
}

// Apply for the executables list only; the switches above it have already committed.
void CapabilityPanel::ApplyProcessSettings()
{
    if (refreshing)
    {
        return;
    }
    auto settings = session.Capabilities().process;
    settings.approvedExecutables.clear();
    const QStringList lines = approvedExecutablesEdit->toPlainText().split('\n');
    for (const QString& line : lines)
    {
        const QString text = line.trimmed();
        if (text.isEmpty())
        {
            continue;
        }
        const std::filesystem::path executable(text.toStdWString());
        if (!executable.is_absolute())
        {
            const QString message = QString("Use an absolute executable path: %1").arg(text);
            processStatusLabel->setText(message);
            SetStatus(message, true);
            approvedExecutablesEdit->setFocus();
            return;
        }
        settings.approvedExecutables.push_back(executable);
    }
    const auto result = session.SetProcessSettings(settings);
    if (result.succeeded)
    {
        processSettingsDirty = false;
    }
    Refresh();
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    if (!result.succeeded)
    {
        processStatusLabel->setText(QString::fromStdString(result.message));
    }
}

void CapabilityPanel::CommitProcessFlag(const std::function<void(revia::process::ProcessSettings&)>& change)
{
    auto settings = session.Capabilities().process;
    change(settings);
    const auto result = session.SetProcessSettings(settings);
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::ApplyCameraSettings()
{
    if (refreshing) return;
    autonomousCameraCheck->setEnabled(cameraCheck->isChecked());
    const auto result = session.SetCameraAccess(
        cameraCheck->isChecked(), autonomousCameraCheck->isChecked());
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::ApplyDesktopControlSettings()
{
    if (refreshing) return;
    using InputScope =
        revia::actions::CapabilitySettings::DesktopControl::InputScope;
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
        session.StopDesktopControl("stopped from Settings, Permissions");
        SetStatus("Desktop control is stopped. Nothing further will be typed or clicked.");
    }
    Refresh();
}

void CapabilityPanel::ShowDiscovery(const revia::actions::windows::ApplicationControlInventory& inventory)
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
        QString::fromStdString(inventory.application + " — " + inventory.windowTitle) +
        QStringLiteral(". Select only controls %1 should be allowed to change.").arg(CompanionName()));
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
    SetTone(statusLabel, text, error);
}

// Internet results also show inside the internet group, which may be in another Settings section
// than the panel's own status line.
void CapabilityPanel::ShowInternetResult(const QString& text, const bool error)
{
    SetStatus(text, error);
    SetTone(internetStatusLabel, text, error);
    internetStatusLabel->setVisible(!text.isEmpty());
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
    automaticLookupCheck->setEnabled(internetCheck->isChecked());
    visibleBrowserCheck->setEnabled(internetCheck->isChecked());
    autonomousResearchCheck->setEnabled(
        internetCheck->isChecked() && visibleBrowserCheck->isChecked());
    const auto result = session.SetInternetAccess(
        internetCheck->isChecked(), automaticLookupCheck->isChecked());
    ShowInternetResult(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::ApplyBrowserSettings()
{
    if (refreshing) return;
    const auto result = session.SetInternetBrowser(
        visibleBrowserCheck->isChecked(),
        autonomousResearchCheck->isChecked());
    ShowInternetResult(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}
