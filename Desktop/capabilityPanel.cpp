#include "capabilityPanel.h"
#include "settingsCatalog.h"
#include "theme.h"
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

// Horizontal padding inside a card. Rows run edge to edge and pad themselves, as a Settings row
// does, so a permission's switch sits where every other switch in Settings sits.
constexpr int CardPadding = 16;
constexpr const char* CardRowsProperty = "reviaCardRows";

QFrame* MakeCard(const QString& objectName)
{
    auto* card = new QFrame();
    card->setObjectName(objectName);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(CardPadding, CardPadding, CardPadding, CardPadding);
    layout->setSpacing(0);
    return card;
}

// A card of permission rows: the rows span its width with a divider between them, and anything
// else in it goes in a padded block (PaddedBlock).
QFrame* MakeRowCard(const QString& objectName)
{
    QFrame* card = MakeCard(objectName);
    card->layout()->setContentsMargins(0, 0, 0, 0);
    card->setProperty(CardRowsProperty, 0);
    return card;
}

// A card under its title, the way every Settings group is shown: a 13/600 title over the card,
// an optional 12px line under it, and the card 8px below. subtitleOut receives that line when
// the caller rewords it later.
QWidget* TitledGroup(QFrame* card, const QString& title, const QString& subtitle = QString(), QLabel** subtitleOut = nullptr)
{
    auto* group = new QWidget();
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* titleLabel = new QLabel(title, group);
    titleLabel->setObjectName("permGroupTitle");
    layout->addWidget(titleLabel);
    auto* subtitleLabel = new QLabel(subtitle, group);
    subtitleLabel->setObjectName("permGroupSubtitle");
    subtitleLabel->setWordWrap(true);
    subtitleLabel->setVisible(!subtitle.isEmpty() || subtitleOut != nullptr);
    layout->addSpacing(2);
    layout->addWidget(subtitleLabel);
    if (subtitleOut != nullptr)
        *subtitleOut = subtitleLabel;
    layout->addSpacing(8);
    layout->addWidget(card, 1);
    return group;
}

// A block inside a row card for what is not a row (notes, lists, buttons), padded like the rows.
QVBoxLayout* PaddedBlock(QFrame* card)
{
    auto* block = new QVBoxLayout();
    block->setContentsMargins(CardPadding, 0, CardPadding, 0);
    block->setSpacing(0);
    qobject_cast<QVBoxLayout*>(card->layout())->addLayout(block);
    return block;
}

QFrame* MakeSeparator()
{
    auto* line = new QFrame();
    line->setObjectName("rowSeparator");
    line->setFixedHeight(1);
    return line;
}

// A permission row, after a divider when another row comes before it in the card.
void AddRow(QFrame* card, QFrame* row)
{
    auto* layout = qobject_cast<QVBoxLayout*>(card->layout());
    const int rows = card->property(CardRowsProperty).toInt();
    if (rows > 0)
        layout->addWidget(MakeSeparator());
    card->setProperty(CardRowsProperty, rows + 1);
    layout->addWidget(row);
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

const QString ChangeBack = QStringLiteral(" You can change this back here at any time.");

// A dependent switch that cannot be used now takes its row's text with it, greyed the way a
// disabled Settings row is, and the row says why, in a line of its own and on hover, as a
// Settings row does ("Turn on Internet lookups first").
void SetReachable(QCheckBox* toggle, const bool reachable, const QString& reason)
{
    toggle->setEnabled(reachable);
    QWidget* row = toggle->parentWidget();
    if (row == nullptr || !row->property(RowTargetProperty).isValid())
        return;
    const QString why = reachable ? QString() : reason;
    row->setToolTip(why);
    toggle->setToolTip(why);
    if (auto* line = row->findChild<QLabel*>("permRowReason"))
    {
        line->setText(why);
        line->setVisible(!why.isEmpty());
    }
    const QVariant shown = row->property("reachable");
    if (shown.isValid() && shown.toBool() == reachable)
        return;
    row->setProperty("reachable", reachable);
    for (QLabel* label : row->findChildren<QLabel*>())
    {
        label->style()->unpolish(label);
        label->style()->polish(label);
    }
}
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
    if (!companionName.isEmpty())
        return companionName;
    const QString name = QString::fromStdString(session.DisplayName());
    return name.isEmpty() ? QStringLiteral("Revia") : name;
}

QString CapabilityPanel::Label(const ToggleSwitch* toggle) const
{
    const auto* descriptor = revia::desktop::FindSetting(toggle->objectName().toStdString());
    return descriptor != nullptr ? revia::desktop::WithCompanionName(descriptor->label, CompanionName()) : toggle->accessibleName();
}

QString CapabilityPanel::NeedsFirst(const std::initializer_list<const ToggleSwitch*> needed, const bool anyOne) const
{
    QStringList off;
    for (const ToggleSwitch* toggle : needed)
    {
        if (anyOne || !toggle->isChecked())
            off.push_back(Label(toggle));
    }
    if (off.isEmpty())
        return QString();
    const QString last = off.takeLast();
    const QString joined = off.isEmpty() ? last : off.join(QStringLiteral(", ")) + (anyOne ? QStringLiteral(" or ") : QStringLiteral(" and ")) + last;
    return QStringLiteral("Turn on %1 first").arg(joined);
}

void CapabilityPanel::SetCompanionName(const QString& name)
{
    if (name.isEmpty() || name == companionName)
        return;
    companionName = name;
    for (std::size_t index = 0; index < rowTexts.size(); ++index)
        RenderRowText(index);
    if (gatedNotice != nullptr)
    {
        gatedNotice->setText(QStringLiteral("These extend %1 beyond approved applications. Policy, confirmation and the audit log "
                                            "still apply to every action, but the scope they are checked against grows.")
                                 .arg(name));
    }
    if (approvedHelp != nullptr)
    {
        approvedHelp->setText(QStringLiteral("An application starts with read-only inspection and no controls. Only the controls listed "
                                             "here may be invoked or edited by %1.")
                                  .arg(name));
    }
}

QString CapabilityPanel::Warning(const ToggleSwitch* toggle) const
{
    const auto* descriptor = revia::desktop::FindSetting(toggle->objectName().toStdString());
    return descriptor != nullptr ? revia::desktop::WithCompanionName(descriptor->warning, CompanionName()) : QString();
}

// One permission: the catalog's label and one-line help, risk badges, an optional second help
// line, the reason it cannot be used now (when it cannot), and the switch on the right. The switch
// must already carry its objectName.
QFrame* CapabilityPanel::MakeRow(ToggleSwitch* toggle, const QString& tip, const bool nested, const QString& riskText)
{
    using revia::desktop::SettingRisk;
    const auto* descriptor = revia::desktop::FindSetting(toggle->objectName().toStdString());

    RowText text;
    text.toggle = toggle;
    text.tip = tip;
    text.row = new QFrame();
    QFrame* row = text.row;
    row->setObjectName(nested ? "permRowNested" : "permRow");
    row->setProperty(RowTargetProperty, QVariant::fromValue<QObject*>(toggle));
    auto* layout = new QHBoxLayout(row);
    // Dependent rows are not indented, as in the rest of Settings: being disabled while the switch
    // they depend on is off says so.
    layout->setContentsMargins(CardPadding, 12, CardPadding, 12);
    layout->setSpacing(16);

    auto* column = new QVBoxLayout();
    column->setSpacing(4);
    text.label = new QLabel();
    text.label->setObjectName(nested ? "permLabelNested" : "permLabel");
    text.label->setWordWrap(true);
    column->addWidget(text.label);
    if (descriptor != nullptr && !descriptor->description.isEmpty())
    {
        text.help = new QLabel();
        text.help->setObjectName("permHelp");
        text.help->setWordWrap(true);
        column->addWidget(text.help);
    }
    if (!tip.isEmpty())
    {
        // What used to hide behind an "i" tooltip is now a visible second line.
        text.detail = new QLabel();
        text.detail->setObjectName("permRowHelp");
        text.detail->setWordWrap(true);
        column->addWidget(text.detail);
    }
    auto* reason = new QLabel();
    reason->setObjectName("permRowReason");
    reason->setWordWrap(true);
    reason->hide();
    column->addWidget(reason);
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
        column->addLayout(badgeRow);
    }
    text.badges = badges;
    layout->addLayout(column, 1);
    layout->addWidget(toggle, 0, Qt::AlignVCenter);
    rowTexts.push_back(std::move(text));
    RenderRowText(rowTexts.size() - 1);
    return row;
}

void CapabilityPanel::RenderRowText(const std::size_t index)
{
    RowText& text = rowTexts[index];
    const auto* descriptor = revia::desktop::FindSetting(text.toggle->objectName().toStdString());
    const QString name = CompanionName();
    const QString label = descriptor != nullptr ? revia::desktop::WithCompanionName(descriptor->label, name) : text.toggle->objectName();
    const QString help = descriptor != nullptr ? revia::desktop::WithCompanionName(descriptor->description, name) : QString();
    const QString detail = revia::desktop::WithCompanionName(text.tip, name);
    text.label->setText(label);
    if (text.help != nullptr)
        text.help->setText(help);
    if (text.detail != nullptr)
        text.detail->setText(detail);
    QString description = help;
    if (!detail.isEmpty())
        description += (description.isEmpty() ? QString() : QStringLiteral(" ")) + detail;
    if (!text.badges.isEmpty())
        description += QStringLiteral(" (") + text.badges.join(QStringLiteral(", ")) + QLatin1Char(')');
    text.row->setAccessibleName(label);
    text.row->setAccessibleDescription(description);
    text.toggle->setAccessibleName(label);
    text.toggle->setAccessibleDescription(description);
}

CapabilityPanel::CapabilityPanel(revia::runtime::ReviaSession& inputSession, DiscoveryRequest inputDiscoveryRequest, QWidget* parent)
    : QWidget(parent),
      session(inputSession),
      requestDiscovery(std::move(inputDiscoveryRequest))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    // Groups sit as far apart as Settings' own.
    layout->setSpacing(24);

    // No title or banner of its own: Settings › Permissions names the section and introduces it,
    // and the internet cards live in Settings › Internet. Requested tasks is one Settings-style
    // row in a group of its own: the choice on the right, a one-line description on the left.
    auto* taskApprovals = new QWidget(this);
    auto* taskLayout = new QVBoxLayout(taskApprovals);
    taskLayout->setContentsMargins(0, 0, 0, 0);
    taskLayout->setSpacing(8);
    auto* taskGroupTitle = new QLabel("Tasks", taskApprovals);
    taskGroupTitle->setObjectName("permGroupTitle");
    taskLayout->addWidget(taskGroupTitle);
    auto* taskCard = new QFrame(taskApprovals);
    taskCard->setObjectName("permCard");
    auto* taskRow = new QHBoxLayout(taskCard);
    taskRow->setContentsMargins(CardPadding, 12, CardPadding, 12);
    taskRow->setSpacing(16);
    auto* taskText = new QVBoxLayout();
    taskText->setSpacing(4);
    auto* taskTitle = new QLabel("Requested tasks", taskCard);
    taskTitle->setObjectName("permLabel");
    taskText->addWidget(taskTitle);
    auto* taskHelp = new QLabel("How a task you ask for starts. Your permissions apply either way.", taskCard);
    taskHelp->setObjectName("permHelp");
    taskHelp->setWordWrap(true);
    taskText->addWidget(taskHelp);
    taskRow->addLayout(taskText, 1);
    taskApprovalMode = new QComboBox(taskCard);
    taskApprovalMode->setObjectName("taskApprovalMode");
    taskApprovalMode->setAccessibleName("Requested tasks");
    const QString taskDetail = "Task approval covers routine steps and messages you explicitly request. Free mode skips that "
                               "initial approval. Existing permissions still apply; work outside the task's scope stops. "
                               "Background work gets no task approval.";
    taskApprovalMode->setToolTip(taskDetail);
    taskApprovalMode->setAccessibleDescription(taskDetail);
    taskApprovalMode->setFixedWidth(240);
    // The list opens wide enough for its longest choice.
    taskApprovalMode->view()->setMinimumWidth(340);
    using ExecutionMode = revia::actions::ExecutionMode;
    taskApprovalMode->addItem("Approve once at the start", static_cast<int>(ExecutionMode::Supervised));
    taskApprovalMode->addItem("Free — use granted permissions without asking", static_cast<int>(ExecutionMode::OwnerFullAccess));
    taskApprovalMode->addItem("Approved scope only", static_cast<int>(ExecutionMode::ApprovedScope));
    taskApprovalMode->addItem("Actions disabled", static_cast<int>(ExecutionMode::Disabled));
    // Changed only once clicked or tabbed into; see eventFilter.
    taskApprovalMode->setFocusPolicy(Qt::StrongFocus);
    taskApprovalMode->installEventFilter(this);
    taskRow->addWidget(taskApprovalMode, 0, Qt::AlignVCenter);
    taskLayout->addWidget(taskCard);
    layout->addWidget(taskApprovals);
    connect(taskApprovalMode, &QComboBox::activated, this, [this](const int index)
    {
        if (refreshing) return;
        const auto mode = static_cast<ExecutionMode>(taskApprovalMode->itemData(index).toInt());
        const auto commit = [this, mode]()
        {
            const auto result = session.SetExecutionMode(mode);
            SetStatus(QString::fromStdString(result.message), !result.succeeded);
            Refresh();
        };
        if (mode == ExecutionMode::OwnerFullAccess && session.Capabilities().mode != ExecutionMode::OwnerFullAccess)
        {
            // Free mode drops the approval every requested task asks for at the start, so it asks
            // first like the risky switches do; the saved mode stays selected until the answer.
            Refresh();
            AskToConfirm(QStringLiteral("Turn on Free mode?"),
                QStringLiteral("Tasks you ask for start without asking you first. The permissions you granted still "
                               "apply, and work outside a task's scope still stops."),
                commit);
            return;
        }
        commit();
    });

    // ------------------------------------------------------------ internet group
    // Built inside its own container so the Settings dialog can show it in the Internet section;
    // until then it sits here, and the panel works on its own.
    internetGroup = new QWidget(this);
    auto* internetLayout = new QVBoxLayout(internetGroup);
    internetLayout->setContentsMargins(0, 0, 0, 0);
    internetLayout->setSpacing(24);
    internetLayout->addWidget(TitledGroup(CreateInternetCard(), QStringLiteral("Internet and research"),
        QStringLiteral("Read-only and rate-limited. Results are grounding, never instructions.")));
    internetLayout->addWidget(TitledGroup(CreateInteractiveBrowserCard(), QStringLiteral("Interactive browser"),
        QStringLiteral("Navigate pages, click controls and fill forms on sites you approve.")));
    internetStatusLabel = new QLabel(internetGroup);
    internetStatusLabel->setObjectName("settingStatus");
    internetStatusLabel->setWordWrap(true);
    internetStatusLabel->hide();
    internetLayout->addWidget(internetStatusLabel);
    layout->addWidget(internetGroup);

    auto* cards = new QGridLayout();
    cards->setObjectName("permissionCards");
    cards->setHorizontalSpacing(24);
    cards->setVerticalSpacing(24);
    cards->setColumnStretch(0, 1);
    cards->setColumnStretch(1, 1);

    // ------------------------------------------------------------ camera
    // Its rows say what the camera may do (one still photo, never video), so no subtitle repeats it.
    auto* cameraCard = MakeRowCard("permCard");
    cameraCheck = new ToggleSwitch(ToggleSwitch::Accent::Cyan, this);
    cameraCheck->setObjectName("cameraCheck");
    autonomousCameraCheck = new ToggleSwitch(ToggleSwitch::Accent::Violet, this);
    autonomousCameraCheck->setObjectName("autonomousCameraCheck");
    AddRow(cameraCard, MakeRow(cameraCheck, "The camera is opened for that one photo and closed again at once."));
    AddRow(cameraCard, MakeRow(autonomousCameraCheck,
        "A separate permission. Without it the camera is used only when you ask.", true));
    qobject_cast<QVBoxLayout*>(cameraCard->layout())->addStretch();
    cards->addWidget(TitledGroup(cameraCard, QStringLiteral("Camera")), 0, 0);

    // ------------------------------------------------------------ pointer and keyboard
    auto* desktopCard = MakeRowCard("permCard");
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
    QVBoxLayout* desktopBlock = PaddedBlock(desktopCard);
    desktopBlock->addSpacing(8);
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
    desktopBlock->addWidget(stopBar);
    desktopBlock->addSpacing(12);
    desktopLayout->addStretch();
    cards->addWidget(TitledGroup(desktopCard, QStringLiteral("Pointer and keyboard"),
                         QStringLiteral("Synthesized input is indistinguishable from you typing. Confined to approved applications "
                                        "unless that confinement is lifted.")),
        0, 1);

    // ------------------------------------------------------------ gated
    auto* gatedCard = MakeRowCard("gatedCard");
    gatedNotice = new QLabel(QStringLiteral(
        "These extend %1 beyond approved applications. Policy, confirmation and the "
        "audit log still apply to every action, but the scope they are checked "
        "against grows.").arg(CompanionName()));
    gatedNotice->setObjectName("gatedNotice");
    gatedNotice->setWordWrap(true);
    auto* gatedLayout = qobject_cast<QVBoxLayout*>(gatedCard->layout());
    QVBoxLayout* gatedBlock = PaddedBlock(gatedCard);
    gatedBlock->addSpacing(12);
    gatedBlock->addWidget(gatedNotice);
    gatedBlock->addSpacing(4);

    wholeDesktopCheck = new ToggleSwitch(ToggleSwitch::Accent::Amber, this);
    wholeDesktopCheck->setObjectName("wholeDesktopCheck");
    commandSurfaceCheck = new ToggleSwitch(ToggleSwitch::Accent::Red, this);
    commandSurfaceCheck->setObjectName("commandSurfaceCheck");
    autonomousDesktopCheck = new ToggleSwitch(ToggleSwitch::Accent::Violet, this);
    autonomousDesktopCheck->setObjectName("autonomousDesktopCheck");
    AddRow(gatedCard, MakeRow(wholeDesktopCheck,
        "The pointer goes anywhere and the keyboard goes to whatever has focus, the way it does for you.", false,
        "Removes confinement"));
    AddRow(gatedCard, MakeRow(commandSurfaceCheck,
        "Off by default. Typing into a command interpreter is model text reaching a shell, whichever window it "
        "arrived through.", false, "Command surface"));
    AddRow(gatedCard, MakeRow(autonomousDesktopCheck,
        "A separate permission. Without it desktop work happens only as part of something you asked for."));
    gatedLayout->addStretch();
    cards->addWidget(TitledGroup(gatedCard, QStringLiteral("Gated controls"),
                         QStringLiteral("Each one removes a boundary the other permissions rely on. Off by default.")),
        1, 0);

    // ------------------------------------------------------------ commands
    auto* processCard = MakeRowCard("permCard");
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
    QVBoxLayout* processLayout = PaddedBlock(processCard);
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
    // One entry per line, so Tab moves on instead of typing a tab: in Settings, Esc keeps the
    // typed list and the focus, and Tab is the keyboard's way out.
    approvedExecutablesEdit->setTabChangesFocus(true);
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
    processLayout->addSpacing(12);
    cards->addWidget(TitledGroup(processCard, QStringLiteral("Commands"), QStringLiteral("Run approved programs with bounded time and output.")),
        1, 1);
    layout->addLayout(cards);

    // Whole-row clicking for every permission built above, the internet group included.
    for (QFrame* row : findChildren<QFrame*>())
    {
        if (row->property(RowTargetProperty).isValid()) row->installEventFilter(this);
    }

    // ------------------------------------------------------------ approved + discovery
    auto* body = new QHBoxLayout();
    body->setObjectName("permissionApplications");
    body->setSpacing(24);

    auto* approvedCard = MakeCard("permCard");
    auto* approvedColumn = qobject_cast<QVBoxLayout*>(approvedCard->layout());
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
    body->addWidget(TitledGroup(approvedCard, QStringLiteral("Approved applications and controls"),
                        QStringLiteral("An application starts with read-only inspection and no controls. Only the controls listed "
                                       "here may be invoked or edited by %1.")
                            .arg(CompanionName()),
                        &approvedHelp),
        1);

    auto* discoveryCard = MakeCard("permCard");
    auto* discoveryColumn = qobject_cast<QVBoxLayout*>(discoveryCard->layout());
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
    body->addWidget(TitledGroup(discoveryCard, QStringLiteral("Foreground control discovery"),
                        QStringLiteral("Minimize Revia and inspect the application underneath. Discovery changes no permission."),
                        &discoveryLabel),
        1);
    layout->addLayout(body, 1);

    statusLabel = new QLabel(
        "Permissions persist in RuntimeData/Capabilities/capabilities.json.", this);
    statusLabel->setObjectName("settingStatus");
    statusLabel->setProperty("tone", "info");
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    // Turning on a risky switch asks first; turning anything off never does. The question carries
    // the switch's own warning; note adds a sentence only where one more fact matters.
    const auto confirmFirst = [this](ToggleSwitch* toggle, const bool on, const QString& note, std::function<void()> apply)
    {
        if (refreshing) return;
        if (on)
            ConfirmTurnOn(toggle, note, std::move(apply));
        else
            apply();
    };
    connect(internetCheck, &QCheckBox::toggled, this, [this, confirmFirst](const bool on)
    {
        confirmFirst(internetCheck, on, QString(), [this]() { ApplyInternetSettings(internetCheck); });
    });
    connect(automaticLookupCheck, &QCheckBox::toggled, this, [this]() { ApplyInternetSettings(automaticLookupCheck); });
    connect(visibleBrowserCheck, &QCheckBox::toggled, this, [this, confirmFirst](const bool on)
    {
        confirmFirst(visibleBrowserCheck, on, QString(), [this]() { ApplyBrowserSettings(visibleBrowserCheck); });
    });
    connect(autonomousResearchCheck, &QCheckBox::toggled, this, [this, confirmFirst](const bool on)
    {
        confirmFirst(autonomousResearchCheck, on, QString(), [this]() { ApplyBrowserSettings(autonomousResearchCheck); });
    });
    connect(cameraCheck, &QCheckBox::toggled, this, [this, confirmFirst](const bool on)
    {
        confirmFirst(cameraCheck, on, QString(), [this]() { ApplyCameraSettings(cameraCheck); });
    });
    connect(autonomousCameraCheck, &QCheckBox::toggled, this, [this, confirmFirst](const bool on)
    {
        confirmFirst(autonomousCameraCheck, on, QString(), [this]() { ApplyCameraSettings(autonomousCameraCheck); });
    });
    for (ToggleSwitch* toggle : {processCheck, taskProcessCheck, commandInterpreterCheck})
    {
        connect(toggle, &QCheckBox::toggled, this, [this, toggle, confirmFirst](const bool on)
        {
            confirmFirst(toggle, on, QString(), [this, toggle]()
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
            // The first hand turned on is the moment desktop control as a whole begins, and the way
            // to stop it is worth knowing before it does.
            const bool hand = toggle == pointerCheck || toggle == keyboardCheck || toggle == launchCheck;
            const QString note = hand && !session.Capabilities().desktopControl.AnyEnabled()
                                     ? QStringLiteral("Hold Ctrl+Alt+Shift at any time to stop it.")
                                     : QString();
            confirmFirst(toggle, on, note, [this, toggle]() { ApplyDesktopControlSettings(toggle); });
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
    auto* internetCard = MakeRowCard("permCard");
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
    auto* card = MakeRowCard("permCard");
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
    QVBoxLayout* cardLayout = PaddedBlock(card);
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
    approvedOriginsEdit->setTabChangesFocus(true); // as for approvedExecutablesEdit
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
    cardLayout->addSpacing(12);

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

void CapabilityPanel::ConfirmTurnOn(ToggleSwitch* toggle, const QString& note, std::function<void()> apply)
{
    {
        const QSignalBlocker blocker(toggle);
        toggle->setChecked(false);
    }
    const auto* descriptor = revia::desktop::FindSetting(toggle->objectName().toStdString());
    const QString label = Label(toggle);
    const QString warning = Warning(toggle);
    const QPointer<QCheckBox> target(toggle);
    const QString question = descriptor != nullptr
                                 ? revia::desktop::WithCompanionName(revia::desktop::ConfirmQuestion(*descriptor, true), CompanionName())
                                 : QStringLiteral("Turn on %1?").arg(label);
    // Title, consequence, Turn on and Cancel, as in a Windows task dialog; nothing hides behind a
    // Show Details button.
    QString consequence = warning.isEmpty() ? label : warning;
    if (!note.isEmpty())
        consequence += QLatin1Char(' ') + note;
    AskToConfirm(question, consequence,
        [target, apply = std::move(apply)]()
        {
            if (target == nullptr || !target->isEnabled())
                return;
            {
                const QSignalBlocker blocker(target);
                target->setChecked(true);
            }
            apply();
        });
}

void CapabilityPanel::AskToConfirm(const QString& question, const QString& consequence, std::function<void()> accepted)
{
    auto* box = new QMessageBox(this);
    box->setObjectName("settingConfirmDialog");
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    box->setIcon(QMessageBox::Warning);
    // The question is the message itself, not only the caption (small or missing on many setups).
    box->setWindowTitle(question);
    box->setTextFormat(Qt::PlainText);
    box->setText(question);
    box->setInformativeText(consequence + ChangeBack);
    QPushButton* accept = box->addButton("Turn on", QMessageBox::AcceptRole);
    // Added to the button box, the button is already polished: the property needs a repolish.
    accept->setProperty("destructive", true);
    accept->style()->unpolish(accept);
    accept->style()->polish(accept);
    QPushButton* cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(cancel);
    box->setEscapeButton(cancel);
    connect(box, &QMessageBox::finished, this, [box, accept, accepted = std::move(accepted)](int)
    {
        if (box->clickedButton() == accept)
            accepted();
    });
    revia::desktop::theme::PolishQuestion(box);
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
        SetReachable(toggle, browser.enabled, NeedsFirst({interactiveBrowserCheck}));
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
    // Built before her session started, the rows said "Revia"; her own name arrives with it.
    SetCompanionName(QString::fromStdString(session.DisplayName()));
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
    SetReachable(automaticLookupCheck, settings.internet.enabled, NeedsFirst({internetCheck}));
    SetReachable(visibleBrowserCheck, settings.internet.enabled, NeedsFirst({internetCheck}));
    SetReachable(autonomousResearchCheck, settings.internet.enabled && settings.internet.visibleBrowser,
        NeedsFirst({internetCheck, visibleBrowserCheck}));
    cameraCheck->setChecked(settings.camera.enabled);
    autonomousCameraCheck->setChecked(settings.camera.autonomousCapture);
    SetReachable(autonomousCameraCheck, settings.camera.enabled, NeedsFirst({cameraCheck}));
    processCheck->setChecked(settings.process.enabled);
    taskProcessCheck->setChecked(settings.process.allowTaskExecution);
    commandInterpreterCheck->setChecked(settings.process.allowCommandInterpreters);
    // Shells and delegated use are grants inside running programs at all.
    SetReachable(taskProcessCheck, settings.process.enabled, NeedsFirst({processCheck}));
    SetReachable(commandInterpreterCheck, settings.process.enabled, NeedsFirst({processCheck}));
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
    SetReachable(rawCoordinateCheck, desktop.pointer, NeedsFirst({pointerCheck}));
    visualTargetCheck->setChecked(desktop.visualTargeting);
    SetReachable(visualTargetCheck, desktop.pointer, NeedsFirst({pointerCheck}));
    wholeDesktopCheck->setChecked(desktop.scope ==
        revia::actions::CapabilitySettings::DesktopControl::InputScope::WholeDesktop);
    SetReachable(wholeDesktopCheck, desktop.pointer && desktop.rawCoordinates, NeedsFirst({pointerCheck, rawCoordinateCheck}));
    commandSurfaceCheck->setChecked(desktop.allowCommandSurfaces);
    SetReachable(commandSurfaceCheck, desktop.pointer || desktop.keyboard, NeedsFirst({pointerCheck, keyboardCheck}, true));
    autonomousDesktopCheck->setChecked(desktop.autonomous);
    SetReachable(autonomousDesktopCheck, desktop.AnyEnabled(), NeedsFirst({pointerCheck, keyboardCheck, launchCheck}, true));
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

// Like every switch here, the camera pair commits only the switch that changed, on top of the saved
// permissions: a switch still showing a value from before a change elsewhere (/internet, another
// window) never writes that stale value back, and nothing is granted without its own question.
void CapabilityPanel::ApplyCameraSettings(const ToggleSwitch* changed)
{
    if (refreshing) return;
    const auto stored = session.Capabilities().camera;
    const auto result = session.SetCameraAccess(changed == cameraCheck ? cameraCheck->isChecked() : stored.enabled,
        changed == autonomousCameraCheck ? autonomousCameraCheck->isChecked() : stored.autonomousCapture);
    SetStatus(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::ApplyDesktopControlSettings(const ToggleSwitch* changed)
{
    if (refreshing) return;
    using InputScope =
        revia::actions::CapabilitySettings::DesktopControl::InputScope;
    auto desktop = session.Capabilities().desktopControl;
    const bool on = changed->isChecked();
    if (changed == pointerCheck)
        desktop.pointer = on;
    else if (changed == keyboardCheck)
        desktop.keyboard = on;
    else if (changed == launchCheck)
        desktop.applicationLaunch = on;
    else if (changed == rawCoordinateCheck)
        desktop.rawCoordinates = on;
    else if (changed == visualTargetCheck)
        desktop.visualTargeting = on;
    else if (changed == autonomousDesktopCheck)
        desktop.autonomous = on;
    else if (changed == wholeDesktopCheck)
        desktop.scope = on ? InputScope::WholeDesktop : InputScope::ApprovedApplications;
    else if (changed == commandSurfaceCheck)
        desktop.allowCommandSurfaces = on;
    const auto result = session.SetDesktopControl(desktop.pointer, desktop.keyboard, desktop.applicationLaunch, desktop.rawCoordinates,
        desktop.visualTargeting, desktop.autonomous, desktop.scope, desktop.allowCommandSurfaces);
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

void CapabilityPanel::ApplyInternetSettings(const ToggleSwitch* changed)
{
    if (refreshing) return;
    const auto stored = session.Capabilities().internet;
    const auto result = session.SetInternetAccess(changed == internetCheck ? internetCheck->isChecked() : stored.enabled,
        changed == automaticLookupCheck ? automaticLookupCheck->isChecked() : stored.automaticLookup);
    ShowInternetResult(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}

void CapabilityPanel::ApplyBrowserSettings(const ToggleSwitch* changed)
{
    if (refreshing) return;
    const auto stored = session.Capabilities().internet;
    const auto result = session.SetInternetBrowser(changed == visibleBrowserCheck ? visibleBrowserCheck->isChecked() : stored.visibleBrowser,
        changed == autonomousResearchCheck ? autonomousResearchCheck->isChecked() : stored.autonomousResearch);
    ShowInternetResult(QString::fromStdString(result.message), !result.succeeded);
    Refresh();
}
