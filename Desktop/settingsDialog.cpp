#include "settingsDialog.h"

#include "desktopObjectNames.h"
#include "settingsCatalog.h"
#include "toggleSwitch.h"

#include <QAbstractButton>
#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <set>
#include <tuple>
#include <utility>

namespace revia::desktop
{
namespace
{
namespace names = revia::desktop::names;

// Rows read best at a fixed measure; hosted panels may use the whole page.
constexpr int RowColumnWidth = 680;
constexpr int SectionRole = Qt::UserRole;
constexpr int HitCountRole = Qt::UserRole + 1;
constexpr int StatusMilliseconds = 6000;
constexpr int HighlightMilliseconds = 1200;
constexpr auto GeometryKey = "settingsDialog/geometry";
constexpr auto SavingText = "Saving\xE2\x80\xA6";

QString Name(const std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

void Repolish(QWidget* widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

int RiskRank(const SettingRisk risk)
{
    switch (risk)
    {
        case SettingRisk::Normal: return 0;
        case SettingRisk::Privacy: return 1;
        case SettingRisk::Dangerous: return 2;
    }
    return 0;
}

// Normal rows first, then privacy rows, then risky ones, and never a row ahead of a row in the
// same group that it requires.
std::vector<const SettingDescriptor*> OrderGroup(std::vector<const SettingDescriptor*> pending)
{
    std::stable_sort(pending.begin(), pending.end(),
        [](const SettingDescriptor* left, const SettingDescriptor* right) { return RiskRank(left->risk) < RiskRank(right->risk); });
    std::set<std::string> inGroup;
    for (const SettingDescriptor* row : pending)
        inGroup.insert(row->key);
    std::set<std::string> placed;
    std::vector<const SettingDescriptor*> ordered;
    while (!pending.empty())
    {
        auto ready = std::find_if(pending.begin(), pending.end(), [&](const SettingDescriptor* row)
        {
            return std::all_of(row->requiresAll.begin(), row->requiresAll.end(),
                [&](const std::string& required) { return !inGroup.contains(required) || placed.contains(required); });
        });
        if (ready == pending.end())
            ready = pending.begin(); // a requirement cycle is a catalog mistake; keep the sorted order
        placed.insert((*ready)->key);
        ordered.push_back(*ready);
        pending.erase(ready);
    }
    return ordered;
}

QString SectionIntro(const SettingsSection section)
{
    switch (section)
    {
        case SettingsSection::General: return QStringLiteral("How the Revia window behaves on this PC, whichever companion is active.");
        case SettingsSection::Appearance: return QStringLiteral("How Revia looks on this PC, whichever companion is active.");
        case SettingsSection::Chat:
            return QStringLiteral("How {name} starts conversations, thinks and checks replies. Profile switches change the "
                                  "active profile.");
        case SettingsSection::Voice: return QStringLiteral("Spoken replies, listening and singing. Applies to {name}.");
        case SettingsSection::Vision:
            return QStringLiteral("What {name} may see. Settings marked All companions apply to every companion.");
        case SettingsSection::Internet:
            return QStringLiteral("Web lookups and the interactive browser. These permissions apply to every companion.");
        case SettingsSection::Memory:
            return QStringLiteral("What {name} keeps and how it is found again. Settings marked All companions apply to every "
                                  "companion.");
        case SettingsSection::Models:
            return QStringLiteral("Helper models, the cloud model and pictures. Settings marked All companions apply to every "
                                  "companion.");
        case SettingsSection::Permissions:
            return QStringLiteral("What {name} may do on this PC. Permissions apply to every companion; the Ubuntu workspace "
                                  "starts with {name}.");
        case SettingsSection::Integrations:
            return QStringLiteral("Avatar, streaming, the local API and tool servers. Settings marked All companions apply to "
                                  "every companion.");
        case SettingsSection::Advanced:
            return QStringLiteral("Self-improvement and hardware. Settings marked All companions apply to every companion.");
    }
    return {};
}

// The panel window a section links to (PanelSpec::key and title), if it has one.
std::pair<const char*, const char*> SectionPanel(const SettingsSection section)
{
    switch (section)
    {
        case SettingsSection::Chat: return {"profiles", "Profiles"};
        case SettingsSection::Voice: return {"voice", "Voice"};
        case SettingsSection::Vision: return {"vision", "Vision"};
        case SettingsSection::Internet: return {"internet", "Internet activity"};
        case SettingsSection::Memory: return {"memory", "Memory"};
        case SettingsSection::Integrations: return {"presence", "Presence"};
        case SettingsSection::Advanced: return {"resources", "Resources"};
        default: return {nullptr, nullptr};
    }
}

QString RestartBadgeText(const SettingApply apply)
{
    switch (apply)
    {
        case SettingApply::Restart: return QStringLiteral("Applies after restart");
        case SettingApply::LiveWhenOffRestartWhenOn: return QStringLiteral("Turning on applies after restart");
        case SettingApply::Partial: return QStringLiteral("Partly applies now");
        case SettingApply::Live: break;
    }
    return {};
}

bool Matches(const QString& haystack, const QStringList& terms)
{
    return std::all_of(terms.begin(), terms.end(), [&](const QString& term) { return haystack.contains(term, Qt::CaseInsensitive); });
}

// Paints a section's search hit count at the right edge of its row, in the secondary text colour
// (the theme maps PlaceholderText to it), so the count stays readable on the selected row.
class SectionItemDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        const QString count = index.data(HitCountRole).toString();
        if (count.isEmpty())
            return;
        painter->save();
        painter->setPen(option.palette.color(QPalette::PlaceholderText));
        painter->drawText(option.rect.adjusted(0, 0, -12, 0), Qt::AlignRight | Qt::AlignVCenter, count);
        painter->restore();
    }
};
}

struct SettingsDialog::Row
{
    SettingDescriptor descriptor;
    // Custom, link and appearance rows have no switch; their key is the row's objectName.
    bool hasSwitch = true;
    // Profile override rows carry an inline value control that follows the switch.
    bool inlineValue = false;
    std::vector<std::string> requiresAll;
    Group* group = nullptr;
    QFrame* frame = nullptr;
    QFrame* divider = nullptr;
    QHBoxLayout* line = nullptr;
    QVBoxLayout* text = nullptr;
    QLabel* title = nullptr;
    QLabel* description = nullptr;
    QWidget* badgeRow = nullptr;
    QLabel* restartBadge = nullptr;
    QLabel* warning = nullptr;
    QLabel* status = nullptr;
    ToggleSwitch* toggle = nullptr;
    QWidget* control = nullptr;
    bool value = false;
    bool running = false;
    bool runningKnown = false;
    bool available = true;
    bool busy = false;
    bool matches = true;
    QString unavailableReason;
    QString disabledReason;
    QString descriptionSuffix;
    QString dependencyHint;
    QString backendStatus, backendTone;
    QString persistentStatus, persistentTone;
    QString transientStatus, transientTone;
    quint64 transientSerial = 0;
};

struct SettingsDialog::Group
{
    SettingsSection section = SettingsSection::General;
    QString title;
    bool serverGroup = false;
    QWidget* box = nullptr;
    QLabel* titleLabel = nullptr;
    QFrame* frame = nullptr;
    QVBoxLayout* layout = nullptr;
    QFrame* riskyDivider = nullptr;
    QLabel* riskyHeader = nullptr;
    QLabel* note = nullptr;
    std::vector<Row*> rows;
};

struct SettingsDialog::Section
{
    SettingsSection id = SettingsSection::General;
    QScrollArea* page = nullptr;
    QWidget* content = nullptr;
    QVBoxLayout* body = nullptr;
    QLabel* title = nullptr;
    QLabel* intro = nullptr;
    QPushButton* openPanel = nullptr;
    QListWidgetItem* item = nullptr;
    std::map<QString, QPointer<QWidget>> hosted;
};

SettingsDialog::SettingsDialog(SettingsBackend inputBackend, SettingsHooks inputHooks, QWidget* parent)
    : QDialog(parent),
      backend(std::move(inputBackend)),
      hooks(std::move(inputHooks))
{
    if (hooks.companionName)
        companionName = hooks.companionName();
    BuildFrame();
    BuildSections();
    const QByteArray geometry = QSettings().value(GeometryKey).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    Refresh();
    ShowSection(SettingsSection::General);
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::BuildFrame()
{
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(QStringLiteral("Settings"));
    setAttribute(Qt::WA_QuitOnClose, false);
    setModal(false);
    setMinimumSize(760, 520);
    resize(920, 640);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    navigation = new QWidget(this);
    navigation->setObjectName(QStringLiteral("settingsNav"));
    navigation->setAttribute(Qt::WA_StyledBackground, true);
    navigation->setFixedWidth(200);
    auto* navLayout = new QVBoxLayout(navigation);
    navLayout->setContentsMargins(12, 16, 12, 12);
    navLayout->setSpacing(8);
    search = new QLineEdit(navigation);
    search->setObjectName(QStringLiteral("settingsSearch"));
    search->setPlaceholderText(QStringLiteral("Search settings"));
    search->setAccessibleName(QStringLiteral("Search settings"));
    search->setClearButtonEnabled(true);
    search->installEventFilter(this);
    navLayout->addWidget(search);
    sectionList = new QListWidget(navigation);
    sectionList->setObjectName(QStringLiteral("settingsSections"));
    sectionList->setAccessibleName(QStringLiteral("Settings sections"));
    sectionList->setItemDelegate(new SectionItemDelegate(sectionList));
    sectionList->setFrameShape(QFrame::NoFrame);
    sectionList->setIconSize(QSize(16, 16));
    sectionList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navLayout->addWidget(sectionList, 1);
    root->addWidget(navigation);

    auto* right = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    restartBanner = new QFrame(right);
    restartBanner->setObjectName(QStringLiteral("settingsRestartBanner"));
    auto* bannerLayout = new QHBoxLayout(restartBanner);
    bannerLayout->setContentsMargins(12, 8, 8, 8);
    bannerLayout->setSpacing(8);
    restartLabel = new QLabel(restartBanner);
    restartLabel->setWordWrap(true);
    bannerLayout->addWidget(restartLabel, 1);
    auto* later = new QPushButton(QStringLiteral("Later"), restartBanner);
    later->setObjectName(QStringLiteral("settingsRestartLater"));
    later->setAutoDefault(false);
    bannerLayout->addWidget(later);
    restartNow = new QPushButton(QStringLiteral("Restart now"), restartBanner);
    restartNow->setObjectName(QStringLiteral("settingsRestartNow"));
    restartNow->setAutoDefault(false);
    restartNow->setVisible(static_cast<bool>(hooks.restartApp));
    bannerLayout->addWidget(restartNow);
    // A layout whose only widget is hidden takes no room, margins included.
    auto* bannerSpace = new QVBoxLayout();
    bannerSpace->setContentsMargins(32, 16, 32, 0);
    bannerSpace->addWidget(restartBanner);
    rightLayout->addLayout(bannerSpace);
    restartBanner->hide();

    noResults = new QLabel(right);
    noResults->setObjectName(QStringLiteral("settingsNoResults"));
    noResults->setWordWrap(true);
    noResults->setTextFormat(Qt::PlainText);
    noResults->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    noResults->hide();
    rightLayout->addWidget(noResults);

    stack = new QStackedWidget(right);
    stack->setObjectName(QStringLiteral("settingsStack"));
    stack->setProperty(names::kNavigationStackProperty, true);
    rightLayout->addWidget(stack, 1);
    root->addWidget(right, 1);

    connect(search, &QLineEdit::textChanged, this, [this](const QString& query) { ApplySearch(query); });
    connect(sectionList, &QListWidget::currentRowChanged, this, [this](const int row)
    {
        if (row >= 0 && row < stack->count())
            stack->setCurrentIndex(row);
    });
    connect(later, &QPushButton::clicked, this, [this]()
    {
        restartDismissedAt = PendingRestartCount();
        UpdateRestartBanner();
    });
    connect(restartNow, &QPushButton::clicked, this, [this]()
    {
        if (hooks.restartApp)
            hooks.restartApp();
    });
    auto* find = new QShortcut(QKeySequence::Find, this);
    find->setContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QShortcut::activated, this, [this]()
    {
        search->setFocus(Qt::ShortcutFocusReason);
        search->selectAll();
    });
}

void SettingsDialog::BuildSections()
{
    for (const SettingsSection id : SettingsSections())
    {
        auto section = std::make_unique<Section>();
        section->id = id;
        section->page = new QScrollArea(stack);
        section->page->setObjectName(Name(names::kSettingsSectionPrefix) + SectionId(id));
        section->page->setProperty(names::kRoleProperty, QStringLiteral("settingsSection"));
        section->page->setFrameShape(QFrame::NoFrame);
        section->page->setWidgetResizable(true);
        section->page->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        section->page->viewport()->setAutoFillBackground(false);
        section->content = new QWidget(section->page);
        section->content->setAutoFillBackground(false);
        auto* column = new QVBoxLayout(section->content);
        column->setContentsMargins(32, 24, 32, 32);
        column->setSpacing(0);
        // Title, intro and groups share one column of at most RowColumnWidth, centred once the page
        // is wider; hosted panels are added outside it and take the full width.
        auto* header = new QWidget(section->content);
        header->setMaximumWidth(RowColumnWidth);
        auto* headerLayout = new QVBoxLayout(header);
        headerLayout->setContentsMargins(0, 0, 0, 0);
        headerLayout->setSpacing(0);
        section->title = new QLabel(SectionName(id), header);
        section->title->setObjectName(QStringLiteral("settingsSectionTitle"));
        headerLayout->addWidget(section->title);
        headerLayout->addSpacing(4);
        section->intro = new QLabel(header);
        section->intro->setObjectName(QStringLiteral("settingsSectionIntro"));
        section->intro->setWordWrap(true);
        section->intro->setTextFormat(Qt::PlainText);
        headerLayout->addWidget(section->intro);
        column->addWidget(header);
        const auto [panelKey, panelTitle] = SectionPanel(id);
        if (panelKey != nullptr)
        {
            headerLayout->addSpacing(12);
            section->openPanel = new QPushButton(QStringLiteral("Open %1 window").arg(QString::fromUtf8(panelTitle)), header);
            section->openPanel->setObjectName(Name(names::kSettingsOpenPanelPrefix) + panelKey);
            section->openPanel->setAutoDefault(false);
            section->openPanel->setVisible(static_cast<bool>(hooks.openPanel));
            const std::string key = panelKey;
            connect(section->openPanel, &QPushButton::clicked, this, [this, key]()
            {
                if (hooks.openPanel)
                    hooks.openPanel(key.c_str());
            });
            headerLayout->addWidget(section->openPanel, 0, Qt::AlignLeft);
        }
        column->addSpacing(24);
        section->body = new QVBoxLayout();
        section->body->setContentsMargins(0, 0, 0, 0);
        section->body->setSpacing(24);
        column->addLayout(section->body);
        column->addStretch(1);
        section->page->setWidget(section->content);
        stack->addWidget(section->page);

        section->item = new QListWidgetItem(SectionName(id), sectionList);
        section->item->setData(SectionRole, static_cast<int>(id));
        sections.push_back(std::move(section));
    }

    BuildAppearanceRows(SectionFor(SettingsSection::Appearance));

    // Groups appear in catalog order; the rows inside each are ordered by risk and requirement.
    std::vector<std::pair<Group*, std::vector<const SettingDescriptor*>>> pending;
    for (const SettingDescriptor& descriptor : SettingCatalog())
    {
        if (descriptor.host != SettingHost::Dialog || IsServerTemplate(descriptor))
            continue;
        Group& group = GroupFor(descriptor.section, descriptor.group);
        auto entry = std::find_if(pending.begin(), pending.end(), [&](const auto& candidate) { return candidate.first == &group; });
        if (entry == pending.end())
        {
            pending.emplace_back(&group, std::vector<const SettingDescriptor*>{});
            entry = std::prev(pending.end());
        }
        entry->second.push_back(&descriptor);
    }
    for (auto& [group, descriptors] : pending)
    {
        for (const SettingDescriptor* descriptor : OrderGroup(descriptors))
            AddSwitchRow(*group, *descriptor);
    }
    BuildServerRows();
    ApplyIcons();
}

void SettingsDialog::ApplyIcons()
{
    if (!hooks.icon)
        return;
    for (const auto& section : sections)
        section->item->setIcon(hooks.icon(SectionIcon(section->id)));
    for (QAbstractButton* button : themeButtons->buttons())
        button->setIcon(hooks.icon(button->property("reviaIcon").toString().toUtf8().constData()));
}

void SettingsDialog::BuildAppearanceRows(Section& section)
{
    Group& group = GroupFor(section.id, QStringLiteral("Theme"));

    auto* segment = new QFrame();
    segment->setObjectName(QStringLiteral("themeSegment"));
    auto* segmentLayout = new QHBoxLayout(segment);
    segmentLayout->setContentsMargins(2, 2, 2, 2);
    segmentLayout->setSpacing(2);
    themeButtons = new QButtonGroup(this);
    themeButtons->setExclusive(true);
    const std::array<std::tuple<const char*, const char*, const char*>, 3> choices = {
        std::tuple{"themeSystem", "System", "laptop"}, std::tuple{"themeLight", "Light", "sun"}, std::tuple{"themeDark", "Dark", "moon"}};
    for (int index = 0; index < static_cast<int>(choices.size()); ++index)
    {
        const auto& [objectName, text, icon] = choices[static_cast<std::size_t>(index)];
        auto* button = new QPushButton(QString::fromUtf8(text), segment);
        button->setObjectName(QString::fromUtf8(objectName));
        button->setCheckable(true);
        button->setAutoDefault(false);
        button->setAccessibleName(QStringLiteral("Theme: %1").arg(QString::fromUtf8(text)));
        button->setProperty("reviaIcon", QString::fromUtf8(icon));
        themeButtons->addButton(button, index);
        segmentLayout->addWidget(button);
    }
    connect(themeButtons, &QButtonGroup::idClicked, this, [this](const int id)
    {
        if (!updating && hooks.setThemePreference)
            hooks.setThemePreference(id);
    });
    Row& theme = AddPlainRow(group, QStringLiteral("Theme"), QStringLiteral("Follow the Windows setting, or always use light or dark."),
        segment, QStringLiteral("themeSegmentRow"), {});
    theme.descriptor.keywords = {"dark mode", "light mode", "colour", "color", "appearance"};

    textSizeCombo = new QComboBox();
    textSizeCombo->setObjectName(QStringLiteral("chatTextSizeCombo"));
    textSizeCombo->setAccessibleName(QStringLiteral("Chat text size"));
    textSizeCombo->addItem(QStringLiteral("Small"), 14);
    textSizeCombo->addItem(QStringLiteral("Default"), 15);
    textSizeCombo->addItem(QStringLiteral("Large"), 17);
    textSizeCombo->setCurrentIndex(1);
    connect(textSizeCombo, &QComboBox::activated, this, [this](const int index)
    {
        if (!updating && hooks.setChatTextSize)
            hooks.setChatTextSize(textSizeCombo->itemData(index).toInt());
    });
    Row& textSize = AddPlainRow(group, QStringLiteral("Chat text size"),
        QStringLiteral("The size of chat messages and of the message box."), textSizeCombo, QStringLiteral("chatTextSizeComboRow"), {});
    textSize.descriptor.keywords = {"font", "zoom", "text size", "bigger", "smaller"};
}

void SettingsDialog::BuildServerRows()
{
    // Rebuilt whenever the configured servers change, so the old rows go first. Widgets are
    // deleted later because this can run from inside one of their own signals.
    for (auto& group : groups)
    {
        if (group->serverGroup && group->box)
        {
            group->box->hide();
            group->box->deleteLater();
        }
    }
    std::erase_if(rows, [](const std::unique_ptr<Row>& row) { return row->group->serverGroup; });
    std::erase_if(groups, [](const std::unique_ptr<Group>& group) { return group->serverGroup; });

    serverIds = backend.mcpServers ? backend.mcpServers() : std::vector<std::string>{};
    const SettingDescriptor* enabled = FindSetting("mcp.servers[*].enabled");
    const SettingDescriptor* confirmation = FindSetting("mcp.servers[*].requireConfirmation");
    if (serverIds.empty() || enabled == nullptr || confirmation == nullptr)
    {
        Group& group = GroupFor(SettingsSection::Integrations, QStringLiteral("Tool servers"), true);
        group.note = new QLabel(QStringLiteral("No tool servers are set up in Config/settings.json."), group.frame);
        group.note->setObjectName(QStringLiteral("settingDescription"));
        group.note->setWordWrap(true);
        group.note->setContentsMargins(16, 12, 16, 12);
        group.layout->addWidget(group.note);
        return;
    }
    for (const std::string& id : serverIds)
    {
        const SettingDescriptor enabledRow = ServerRow(*enabled, id);
        Group& group = GroupFor(SettingsSection::Integrations, enabledRow.group, true);
        AddSwitchRow(group, enabledRow);
        AddSwitchRow(group, ServerRow(*confirmation, id));
    }
}

SettingsDialog::Section& SettingsDialog::SectionFor(const SettingsSection id)
{
    return *sections[static_cast<std::size_t>(id)];
}

const SettingsDialog::Section* SettingsDialog::SectionFor(const SettingsSection id) const
{
    const auto index = static_cast<std::size_t>(id);
    return index < sections.size() ? sections[index].get() : nullptr;
}

SettingsDialog::Group& SettingsDialog::GroupFor(const SettingsSection sectionId, const QString& title, const bool serverGroup)
{
    for (auto& group : groups)
    {
        if (group->section == sectionId && group->title == title)
            return *group;
    }
    Section& section = SectionFor(sectionId);
    auto group = std::make_unique<Group>();
    group->section = sectionId;
    group->title = title;
    group->serverGroup = serverGroup;
    group->box = new QWidget(section.content);
    group->box->setMaximumWidth(RowColumnWidth);
    auto* boxLayout = new QVBoxLayout(group->box);
    boxLayout->setContentsMargins(0, 0, 0, 0);
    boxLayout->setSpacing(8);
    group->titleLabel = new QLabel(Expand(title), group->box);
    group->titleLabel->setObjectName(QStringLiteral("settingsGroupTitle"));
    group->titleLabel->setTextFormat(Qt::PlainText);
    boxLayout->addWidget(group->titleLabel);
    group->frame = new QFrame(group->box);
    group->frame->setObjectName(QStringLiteral("settingsGroup"));
    group->layout = new QVBoxLayout(group->frame);
    group->layout->setContentsMargins(0, 0, 0, 0);
    group->layout->setSpacing(0);
    boxLayout->addWidget(group->frame);
    section.body->addWidget(group->box);
    groups.push_back(std::move(group));
    return *groups.back();
}

QFrame* SettingsDialog::MakeRowFrame(Row& row, const QString& objectName)
{
    row.frame = new QFrame(row.group->frame);
    row.frame->setObjectName(objectName);
    row.frame->setProperty(names::kClassProperty, QStringLiteral("settingRow"));
    row.frame->installEventFilter(this);
    row.line = new QHBoxLayout(row.frame);
    row.line->setContentsMargins(16, 12, 16, 12);
    row.line->setSpacing(16);
    row.text = new QVBoxLayout();
    row.text->setSpacing(4);
    row.title = new QLabel(row.frame);
    row.title->setObjectName(QStringLiteral("settingTitle"));
    row.title->setTextFormat(Qt::PlainText);
    row.title->setWordWrap(true);
    row.text->addWidget(row.title);
    row.description = new QLabel(row.frame);
    row.description->setObjectName(QStringLiteral("settingDescription"));
    row.description->setTextFormat(Qt::PlainText);
    row.description->setWordWrap(true);
    row.text->addWidget(row.description);
    row.line->addLayout(row.text, 1);
    return row.frame;
}

SettingsDialog::Row& SettingsDialog::AddSwitchRow(Group& group, const SettingDescriptor& descriptor)
{
    rows.push_back(std::make_unique<Row>());
    Row& row = *rows.back();
    row.descriptor = descriptor;
    row.requiresAll = descriptor.requiresAll;
    row.group = &group;
    row.value = descriptor.defaultValue;
    row.running = descriptor.defaultValue;
    MakeRowFrame(row, QString::fromStdString(descriptor.objectName) + Name(names::kSettingRowSuffix));

    row.badgeRow = new QWidget(row.frame);
    auto* badges = new QHBoxLayout(row.badgeRow);
    badges->setContentsMargins(0, 2, 0, 0);
    badges->setSpacing(6);
    badges->addStretch(1);
    row.text->addWidget(row.badgeRow);
    if (descriptor.apply != SettingApply::Live)
    {
        row.restartBadge = AddBadge(row, descriptor.apply == SettingApply::Partial ? QStringLiteral("partial") : QStringLiteral("restart"),
            RestartBadgeText(descriptor.apply));
    }
    if (descriptor.allCompanions)
        AddBadge(row, QStringLiteral("global"), QStringLiteral("All companions"));
    if (descriptor.risk == SettingRisk::Privacy)
        AddBadge(row, QStringLiteral("privacy"), QStringLiteral("Privacy"));
    if (descriptor.risk == SettingRisk::Dangerous)
        AddBadge(row, QStringLiteral("danger"), QStringLiteral("Risky"));
    if (descriptor.autonomy)
        AddBadge(row, QStringLiteral("autonomy"), QStringLiteral("Autonomy"));
    row.badgeRow->setVisible(row.badgeRow->findChild<QLabel*>() != nullptr);

    row.warning = new QLabel(row.frame);
    row.warning->setObjectName(QStringLiteral("settingWarning"));
    row.warning->setTextFormat(Qt::PlainText);
    row.warning->setWordWrap(true);
    row.warning->hide();
    row.text->addWidget(row.warning);
    row.status = new QLabel(row.frame);
    row.status->setObjectName(QStringLiteral("settingStatus"));
    row.status->setTextFormat(Qt::PlainText);
    row.status->setWordWrap(true);
    row.status->hide();
    row.text->addWidget(row.status);

    const std::string key = descriptor.key;
    if (key == "profile.hasTemperatureOverride" || key == "profile.hasMaxTokensOverride")
    {
        // The override switch and the value it overrides with sit in one row.
        row.inlineValue = true;
        const bool temperature = key == "profile.hasTemperatureOverride";
        QAbstractSpinBox* spin = nullptr;
        if (temperature)
        {
            auto* value = new QDoubleSpinBox(row.frame);
            value->setObjectName(QStringLiteral("profileTemperatureSpin"));
            value->setRange(0.0, 2.0);
            value->setSingleStep(0.05);
            value->setDecimals(2);
            value->setValue(0.7);
            value->setAccessibleName(QStringLiteral("Profile temperature"));
            connect(value, &QDoubleSpinBox::editingFinished, this, [this, key, value]()
                { WriteProfileValue(key, "temperature", value->value()); });
            spin = value;
        }
        else
        {
            auto* value = new QSpinBox(row.frame);
            value->setObjectName(QStringLiteral("profileMaxTokensSpin"));
            value->setRange(1, 32768);
            value->setValue(512);
            value->setSuffix(QStringLiteral(" tokens"));
            value->setAccessibleName(QStringLiteral("Profile reply limit"));
            connect(value, &QSpinBox::editingFinished, this, [this, key, value]()
                { WriteProfileValue(key, "maxTokens", static_cast<double>(value->value())); });
            spin = value;
        }
        spin->setKeyboardTracking(false);
        row.control = spin;
        row.text->addSpacing(4);
        row.text->addWidget(spin, 0, Qt::AlignLeft);
    }

    row.toggle = new ToggleSwitch(ToggleSwitch::Accent::Cyan, row.frame);
    row.toggle->setObjectName(QString::fromStdString(descriptor.objectName));
    row.toggle->setProperty(names::kRoleProperty, QStringLiteral("settingSwitch"));
    row.line->addWidget(row.toggle, 0, Qt::AlignVCenter);
    connect(row.toggle, &QCheckBox::toggled, this, [this, key](const bool checked) { OnToggled(key, checked); });
    SetDisplay(row, row.value);
    RenderRowTexts(row);
    PlaceInGroup(group, row, false);
    return row;
}

SettingsDialog::Row& SettingsDialog::AddPlainRow(Group& group, const QString& label, const QString& description, QWidget* control,
    const QString& rowObjectName, std::vector<std::string> requiresAll)
{
    rows.push_back(std::make_unique<Row>());
    Row& row = *rows.back();
    row.hasSwitch = false;
    row.descriptor.key = rowObjectName.toStdString();
    row.descriptor.section = group.section;
    row.descriptor.group = group.title;
    row.descriptor.label = label;
    row.descriptor.description = description;
    row.requiresAll = std::move(requiresAll);
    row.group = &group;
    MakeRowFrame(row, rowObjectName);
    row.status = new QLabel(row.frame);
    row.status->setObjectName(QStringLiteral("settingStatus"));
    row.status->setTextFormat(Qt::PlainText);
    row.status->setWordWrap(true);
    row.status->hide();
    row.text->addWidget(row.status);
    if (control != nullptr)
    {
        row.control = control;
        control->setParent(row.frame);
        // A wide control (the microphone picker with its buttons) gets the full row under the text.
        if (control->sizeHint().width() > 260)
            row.text->addWidget(control);
        else
            row.line->addWidget(control, 0, Qt::AlignVCenter);
        control->setVisible(true);
    }
    RenderRowTexts(row);
    PlaceInGroup(group, row, true);
    return row;
}

QLabel* SettingsDialog::AddBadge(Row& row, const QString& kind, const QString& text)
{
    auto* badge = new QLabel(text, row.badgeRow);
    badge->setObjectName(QStringLiteral("settingBadge"));
    badge->setProperty("kind", kind);
    badge->setTextFormat(Qt::PlainText);
    auto* layout = static_cast<QHBoxLayout*>(row.badgeRow->layout());
    layout->insertWidget(layout->count() - 1, badge);
    return badge;
}

void SettingsDialog::PlaceInGroup(Group& group, Row& row, const bool beforeRisky)
{
    const bool risky = row.hasSwitch && row.descriptor.risk == SettingRisk::Dangerous;
    // Plain rows are never risky, so they go above the "Risky" header when there is one.
    if (beforeRisky && group.riskyHeader != nullptr)
    {
        const auto firstRisky = std::find_if(group.rows.begin(), group.rows.end(),
            [](const Row* candidate) { return candidate->hasSwitch && candidate->descriptor.risk == SettingRisk::Dangerous; });
        int index = group.layout->indexOf(group.riskyDivider != nullptr ? static_cast<QWidget*>(group.riskyDivider) : group.riskyHeader);
        if (firstRisky != group.rows.begin())
        {
            row.divider = new QFrame(group.frame);
            row.divider->setObjectName(QStringLiteral("settingsDivider"));
            group.layout->insertWidget(index++, row.divider);
        }
        group.layout->insertWidget(index, row.frame);
        group.rows.insert(firstRisky, &row);
        if (group.riskyDivider == nullptr)
        {
            group.riskyDivider = new QFrame(group.frame);
            group.riskyDivider->setObjectName(QStringLiteral("settingsDivider"));
            group.layout->insertWidget(group.layout->indexOf(group.riskyHeader), group.riskyDivider);
        }
        LayoutDividers(group);
        return;
    }
    if (risky && group.riskyHeader == nullptr)
    {
        if (!group.rows.empty())
        {
            group.riskyDivider = new QFrame(group.frame);
            group.riskyDivider->setObjectName(QStringLiteral("settingsDivider"));
            group.layout->addWidget(group.riskyDivider);
        }
        group.riskyHeader = new QLabel(QStringLiteral("Risky"), group.frame);
        group.riskyHeader->setObjectName(QStringLiteral("settingsRiskyHeader"));
        group.riskyHeader->setContentsMargins(16, 12, 16, 0);
        group.layout->addWidget(group.riskyHeader);
    }
    else if (!group.rows.empty())
    {
        row.divider = new QFrame(group.frame);
        row.divider->setObjectName(QStringLiteral("settingsDivider"));
        group.layout->addWidget(row.divider);
    }
    group.layout->addWidget(row.frame);
    group.rows.push_back(&row);
    LayoutDividers(group);
}

void SettingsDialog::LayoutDividers(Group& group)
{
    bool anyVisible = false;
    bool headerShown = false;
    bool afterHeader = false;
    for (Row* row : group.rows)
    {
        const bool visible = row->matches;
        row->frame->setVisible(visible);
        const bool risky = row->hasSwitch && row->descriptor.risk == SettingRisk::Dangerous;
        if (risky && visible && !headerShown)
        {
            headerShown = true;
            afterHeader = true;
            if (group.riskyDivider != nullptr)
                group.riskyDivider->setVisible(anyVisible);
        }
        if (row->divider != nullptr)
            row->divider->setVisible(visible && anyVisible && !afterHeader);
        if (visible)
        {
            anyVisible = true;
            afterHeader = false;
        }
    }
    if (group.riskyHeader != nullptr)
        group.riskyHeader->setVisible(headerShown);
    if (!headerShown && group.riskyDivider != nullptr)
        group.riskyDivider->hide();
    const bool searching = !search->text().trimmed().isEmpty();
    group.box->setVisible(anyVisible || (group.note != nullptr && !searching));
}

SettingsDialog::Row* SettingsDialog::FindRow(const std::string& keyOrObjectName) const
{
    if (keyOrObjectName.empty())
        return nullptr;
    for (const auto& row : rows)
    {
        if (row->descriptor.key == keyOrObjectName || (!row->descriptor.objectName.empty() && row->descriptor.objectName == keyOrObjectName))
            return row.get();
    }
    return nullptr;
}

SettingsDialog::Row* SettingsDialog::FindRowByWidget(const QObject* widget) const
{
    for (const auto& row : rows)
    {
        if (row->frame == widget)
            return row.get();
    }
    return nullptr;
}

QCheckBox* SettingsDialog::Switch(const std::string& keyOrObjectName) const
{
    if (Row* row = FindRow(keyOrObjectName); row != nullptr && row->toggle != nullptr)
        return row->toggle;
    QString objectName = QString::fromStdString(keyOrObjectName);
    if (const SettingDescriptor* descriptor = FindSetting(keyOrObjectName))
        objectName = QString::fromStdString(descriptor->objectName);
    for (const auto& section : sections)
    {
        for (const auto& [slot, widget] : section->hosted)
        {
            if (widget == nullptr)
                continue;
            if (auto* box = widget->findChild<QCheckBox*>(objectName))
                return box;
        }
    }
    return nullptr;
}

std::vector<SettingDescriptor> SettingsDialog::Descriptors() const
{
    std::vector<SettingDescriptor> built;
    for (const auto& row : rows)
    {
        if (row->hasSwitch)
            built.push_back(row->descriptor);
    }
    return built;
}

void SettingsDialog::OnToggled(const std::string& key, const bool checked)
{
    if (updating)
        return;
    Row* row = FindRow(key);
    if (row == nullptr || !row->hasSwitch)
        return;
    const ConfirmWhen confirm = row->descriptor.confirm;
    // Confirmation follows what the switch shows, so an inverted row asks when its positive
    // question is answered yes.
    if ((confirm == ConfirmWhen::TurningOn && checked) || (confirm == ConfirmWhen::TurningOff && !checked))
    {
        AskToConfirm(key, checked);
        return;
    }
    Commit(key, checked);
}

void SettingsDialog::AskToConfirm(const std::string& key, const bool checked)
{
    Row* row = FindRow(key);
    {
        // The switch stays where it was until the owner answers.
        const QSignalBlocker blocker(row->toggle);
        row->toggle->setChecked(!checked);
    }
    const QString label = Expand(row->descriptor.label);
    QString text = Expand(row->descriptor.warning);
    if (text.isEmpty())
        text = Expand(row->descriptor.description);

    auto* box = new QMessageBox(this);
    box->setObjectName(QStringLiteral("settingConfirmDialog"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(QStringLiteral("%1 %2?").arg(checked ? QStringLiteral("Turn on") : QStringLiteral("Turn off"), label));
    box->setTextFormat(Qt::PlainText);
    box->setText(text + QStringLiteral("\n\nYou can change this back here at any time."));
    QPushButton* accept = box->addButton(checked ? QStringLiteral("Turn on") : QStringLiteral("Turn off"), QMessageBox::AcceptRole);
    QPushButton* cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(cancel);
    box->setEscapeButton(cancel);
    connect(box, &QMessageBox::finished, this, [this, key, checked, box, accept](int)
    {
        if (box->clickedButton() != accept)
            return;
        Row* answered = FindRow(key);
        if (answered == nullptr || answered->busy || !answered->frame->isEnabled())
            return;
        {
            const QSignalBlocker blocker(answered->toggle);
            answered->toggle->setChecked(checked);
        }
        Commit(key, checked);
    });
    box->open();
}

void SettingsDialog::Commit(const std::string& key, const bool checked)
{
    Row* row = FindRow(key);
    if (row == nullptr)
        return;
    const SettingDescriptor& descriptor = row->descriptor;
    const bool stored = descriptor.inverted ? !checked : checked;
    if (descriptor.store == SettingStore::Shell)
    {
        // QSettings is synchronous and cannot refuse.
        QSettings().setValue(QString::fromStdString(descriptor.key), stored);
        row->value = stored;
        if (descriptor.apply == SettingApply::Live)
            row->running = stored;
        SetDisplay(*row, stored);
        RenderRowTexts(*row);
        EvaluateDependencies();
        UpdateRestartBanner();
        if (hooks.valueChanged)
            hooks.valueChanged(key, stored);
        return;
    }
    if (!backend.write)
    {
        FinishWrite(key, !checked, std::nullopt, {false, QStringLiteral("Available once Revia has started.")});
        return;
    }
    row->busy = true;
    ShowTransientStatus(*row, QString::fromUtf8(SavingText), QStringLiteral("info"));
    ApplyRowState(*row);
    QPointer<SettingsDialog> guard(this);
    backend.write({descriptor.key, descriptor.store}, stored, [guard, key, checked, stored](SettingWrite result)
    {
        if (guard)
            guard->FinishWrite(key, !checked, stored, result);
    });
}

void SettingsDialog::FinishWrite(const std::string& key, const bool previousDisplay, const std::optional<bool> written,
    const SettingWrite& result)
{
    Row* row = FindRow(key);
    if (row == nullptr)
        return;
    row->busy = false;
    if (!result.succeeded)
    {
        {
            const QSignalBlocker blocker(row->toggle);
            row->toggle->setChecked(previousDisplay);
        }
        ShowTransientStatus(*row, result.message.isEmpty() ? QStringLiteral("The change could not be saved.") : result.message,
            QStringLiteral("error"));
    }
    else
    {
        ShowTransientStatus(*row, result.message.isEmpty() ? QStringLiteral("Saved.") : result.message, QStringLiteral("success"));
        if (written.has_value())
        {
            row->value = *written;
            if (row->descriptor.apply == SettingApply::Live)
                row->running = *written;
        }
        ReadRows({row});
    }
    RenderRowTexts(*row);
    EvaluateDependencies();
    UpdateRestartBanner();
    // Last: the window may refresh the dialog from here, which can rebuild tool-server rows.
    if (result.succeeded && hooks.valueChanged)
    {
        const bool value = row->value;
        hooks.valueChanged(key, value);
    }
}

void SettingsDialog::ResetRow(const std::string& key)
{
    Row* row = FindRow(key);
    if (row == nullptr || !backend.reset)
        return;
    const bool display = row->toggle->isChecked();
    row->busy = true;
    ShowTransientStatus(*row, QString::fromUtf8(SavingText), QStringLiteral("info"));
    ApplyRowState(*row);
    QPointer<SettingsDialog> guard(this);
    backend.reset({row->descriptor.key, row->descriptor.store}, [guard, key, display](SettingWrite result)
    {
        if (guard)
            guard->FinishWrite(key, display, std::nullopt, result);
    });
}

void SettingsDialog::WriteProfileValue(const std::string& key, const std::string& field, const double value)
{
    Row* row = FindRow(key);
    if (row == nullptr || updating || !backend.writeProfileValue)
        return;
    ShowTransientStatus(*row, QString::fromUtf8(SavingText), QStringLiteral("info"));
    QPointer<SettingsDialog> guard(this);
    backend.writeProfileValue(field, value, [guard, key](SettingWrite result)
    {
        if (!guard)
            return;
        if (Row* answered = guard->FindRow(key))
        {
            guard->ShowTransientStatus(*answered,
                result.message.isEmpty() ? (result.succeeded ? QStringLiteral("Saved.") : QStringLiteral("The change could not be saved."))
                                         : result.message,
                result.succeeded ? QStringLiteral("success") : QStringLiteral("error"));
        }
    });
}

void SettingsDialog::ReadRows(const std::vector<Row*>& wanted)
{
    std::vector<SettingKeyRef> keys;
    for (Row* row : wanted)
    {
        if (row->hasSwitch && row->descriptor.store != SettingStore::Shell)
            keys.push_back({row->descriptor.key, row->descriptor.store});
    }
    if (keys.empty())
        return;
    const std::map<std::string, SettingRead> values = backend.readAll ? backend.readAll(keys) : std::map<std::string, SettingRead>{};
    for (Row* row : wanted)
    {
        if (!row->hasSwitch || row->descriptor.store == SettingStore::Shell)
            continue;
        const auto found = values.find(row->descriptor.key);
        if (found == values.end())
        {
            // A backend that cannot read a key yet (before startup) leaves it showing its default.
            row->available = false;
            row->unavailableReason.clear();
            continue;
        }
        row->available = found->second.available;
        row->unavailableReason = found->second.unavailableReason;
        row->value = found->second.value;
        row->running = found->second.running;
        if (!row->busy)
            SetDisplay(*row, row->value);
    }
}

void SettingsDialog::SetDisplay(Row& row, const bool storedValue)
{
    if (row.toggle == nullptr)
        return;
    const QSignalBlocker blocker(row.toggle);
    row.toggle->setChecked(row.descriptor.inverted ? !storedValue : storedValue);
}

bool SettingsDialog::StoredValue(const std::string& key, bool& value) const
{
    if (Row* row = FindRow(key); row != nullptr && row->hasSwitch)
    {
        value = row->value;
        return true;
    }
    // A requirement on a hosted panel's switch (autonomous research) reads what that switch shows.
    if (const QCheckBox* box = Switch(key))
    {
        value = box->isChecked();
        return true;
    }
    return false;
}

void SettingsDialog::EvaluateDependencies()
{
    for (const auto& owned : rows)
    {
        Row& row = *owned;
        QString reason;
        if (row.hasSwitch && !row.available)
        {
            reason = row.unavailableReason.isEmpty() ? QStringLiteral("Available once Revia has started") : row.unavailableReason;
        }
        if (reason.isEmpty())
        {
            for (const std::string& required : row.requiresAll)
            {
                bool on = true;
                if (StoredValue(required, on) && !on)
                {
                    reason = QStringLiteral("Turn on %1 first").arg(LabelFor(required));
                    break;
                }
            }
        }
        if (reason.isEmpty() && !row.descriptor.disabledWhenOn.empty())
        {
            bool on = false;
            if (StoredValue(row.descriptor.disabledWhenOn, on) && on)
                reason = QStringLiteral("Not used while %1 is on").arg(LabelFor(row.descriptor.disabledWhenOn));
        }
        row.disabledReason = reason;
        row.dependencyHint.clear();
        if (!row.descriptor.softRequires.empty())
        {
            bool on = true;
            if (StoredValue(row.descriptor.softRequires, on) && !on)
                row.dependencyHint = QStringLiteral("Works best with %1 on").arg(LabelFor(row.descriptor.softRequires));
        }
        ApplyRowState(row);
        RenderStatus(row);
    }
}

void SettingsDialog::ApplyRowState(Row& row)
{
    const bool reachable = row.disabledReason.isEmpty();
    row.frame->setEnabled(reachable);
    row.frame->setToolTip(row.disabledReason);
    if (row.toggle != nullptr)
    {
        row.toggle->setEnabled(reachable && !row.busy);
        row.toggle->setToolTip(row.disabledReason);
    }
    if (row.inlineValue && row.control != nullptr)
        row.control->setEnabled(reachable && !row.busy && row.toggle->isChecked());
    if (row.warning != nullptr)
    {
        const SettingDescriptor& descriptor = row.descriptor;
        const bool checked = row.toggle->isChecked();
        const bool riskyPosition = descriptor.confirm == ConfirmWhen::TurningOff ? !checked : checked;
        const bool warns = descriptor.risk == SettingRisk::Dangerous || descriptor.confirm != ConfirmWhen::Never;
        row.warning->setVisible(warns && riskyPosition && !descriptor.warning.isEmpty());
    }
}

void SettingsDialog::ShowTransientStatus(Row& row, const QString& text, const QString& tone)
{
    row.transientStatus = text;
    row.transientTone = tone;
    row.transientSerial = ++statusSerial;
    RenderStatus(row);
    if (text == QString::fromUtf8(SavingText))
        return; // replaced when the write finishes
    const std::string key = row.descriptor.key;
    const quint64 serial = row.transientSerial;
    QTimer::singleShot(StatusMilliseconds, this, [this, key, serial]()
    {
        Row* current = FindRow(key);
        if (current == nullptr || current->transientSerial != serial)
            return;
        current->transientStatus.clear();
        RenderStatus(*current);
    });
}

void SettingsDialog::RenderStatus(Row& row)
{
    if (row.status == nullptr)
        return;
    QString text = row.transientStatus;
    QString tone = row.transientTone;
    if (text.isEmpty())
    {
        text = row.persistentStatus;
        tone = row.persistentTone;
    }
    if (text.isEmpty())
    {
        text = row.backendStatus;
        tone = row.backendTone;
    }
    if (text.isEmpty())
    {
        text = row.dependencyHint;
        tone = QStringLiteral("info");
    }
    row.status->setText(text);
    row.status->setVisible(!text.isEmpty());
    if (row.status->property("tone").toString() != tone)
    {
        row.status->setProperty("tone", tone);
        Repolish(row.status);
    }
}

void SettingsDialog::RenderTexts()
{
    for (const auto& row : rows)
        RenderRowTexts(*row);
    for (const auto& group : groups)
        group->titleLabel->setText(Expand(group->title));
    for (const auto& section : sections)
        section->intro->setText(Expand(SectionIntro(section->id)));
}

void SettingsDialog::RenderRowTexts(Row& row)
{
    const SettingDescriptor& descriptor = row.descriptor;
    const QString title = Expand(descriptor.label);
    QString description = Expand(descriptor.description);
    if (row.toggle != nullptr && !row.toggle->isChecked())
    {
        const QString whenOff = DescriptionWhenOff(descriptor);
        if (!whenOff.isEmpty())
            description = whenOff;
    }
    if (!row.descriptionSuffix.isEmpty())
        description += QLatin1Char(' ') + row.descriptionSuffix;
    row.title->setText(title);
    row.description->setText(description);
    row.description->setVisible(!description.isEmpty());
    if (row.warning != nullptr)
        row.warning->setText(Expand(descriptor.warning));
    if (row.toggle != nullptr)
    {
        QStringList badges;
        if (row.badgeRow != nullptr)
        {
            for (const QLabel* badge : row.badgeRow->findChildren<QLabel*>())
                badges.push_back(badge->text());
        }
        row.toggle->setAccessibleName(title);
        row.toggle->setAccessibleDescription(badges.isEmpty() ? description : description + QStringLiteral(" (") + badges.join(QStringLiteral(", ")) + QLatin1Char(')'));
    }
    else if (row.control != nullptr && row.control->accessibleName().isEmpty())
    {
        row.control->setAccessibleName(title);
    }
}

void SettingsDialog::UpdateRestartBanner()
{
    const int pending = PendingRestartCount();
    for (const auto& row : rows)
    {
        if (row->restartBadge == nullptr)
            continue;
        const bool waiting = row->value != row->running && row->available;
        row->restartBadge->setText(waiting ? QStringLiteral("Restart to apply") : RestartBadgeText(row->descriptor.apply));
    }
    if (pending != restartDismissedAt)
        restartDismissedAt = -1;
    restartLabel->setText(QStringLiteral("Restart Revia to apply %1 change%2.").arg(pending).arg(pending == 1 ? "" : "s"));
    restartNow->setVisible(static_cast<bool>(hooks.restartApp));
    restartBanner->setVisible(pending > 0 && restartDismissedAt < 0);
}

int SettingsDialog::PendingRestartCount() const
{
    return static_cast<int>(std::count_if(rows.begin(), rows.end(), [](const std::unique_ptr<Row>& row)
    {
        return row->hasSwitch && row->available && row->descriptor.apply != SettingApply::Live && row->value != row->running;
    }));
}

void SettingsDialog::SetValue(const std::string& key, const bool value)
{
    Row* row = FindRow(key);
    if (row == nullptr || !row->hasSwitch)
        return;
    row->value = value;
    if (row->descriptor.apply == SettingApply::Live || !row->runningKnown)
        row->running = value;
    row->runningKnown = true;
    if (!row->busy)
        SetDisplay(*row, value);
    RenderRowTexts(*row);
    EvaluateDependencies();
    UpdateRestartBanner();
}

void SettingsDialog::SetRowStatus(const std::string& key, const QString& text, const QString& tone)
{
    Row* row = FindRow(key);
    if (row == nullptr)
        return;
    row->persistentStatus = text;
    row->persistentTone = tone.isEmpty() ? QStringLiteral("info") : tone;
    RenderStatus(*row);
}

void SettingsDialog::Refresh()
{
    updating = true;
    if (hooks.companionName)
    {
        const QString name = hooks.companionName();
        if (!name.isEmpty())
            companionName = name;
    }
    profileName = backend.readChoice ? backend.readChoice("profile.displayName") : QString();
    if (backend.mcpServers && backend.mcpServers() != serverIds)
        BuildServerRows();

    QSettings settings;
    std::vector<Row*> stored;
    for (const auto& owned : rows)
    {
        Row& row = *owned;
        if (!row.hasSwitch)
            continue;
        if (row.descriptor.store != SettingStore::Shell)
        {
            stored.push_back(&row);
            continue;
        }
        row.value = settings.value(QString::fromStdString(row.descriptor.key), row.descriptor.defaultValue).toBool();
        // A restart-only Shell row keeps the value it started with as the one in effect.
        if (row.descriptor.apply == SettingApply::Live || !row.runningKnown)
            row.running = row.value;
        row.runningKnown = true;
        row.available = true;
        row.unavailableReason.clear();
        if (row.descriptor.key == "window/closeToTray" && hooks.trayAvailable && !hooks.trayAvailable())
        {
            row.available = false;
            row.unavailableReason = QStringLiteral("No system tray is available");
        }
        if (!row.busy)
            SetDisplay(row, row.value);
    }
    ReadRows(stored);

    if (Row* wakeWord = FindRow("speechRecognition.requireWakeWord"))
        wakeWord->descriptionSuffix = backend.statusText ? backend.statusText(wakeWord->descriptor.key) : QString();
    if (Row* cloud = FindRow("llm.cloud.enabled"))
    {
        cloud->backendStatus = backend.statusText ? backend.statusText(cloud->descriptor.key) : QString();
        cloud->backendTone = cloud->backendStatus.contains(QStringLiteral("missing"), Qt::CaseInsensitive) ? QStringLiteral("warning")
                                                                                                            : QStringLiteral("info");
    }
    if (hooks.themePreference)
    {
        if (QAbstractButton* button = themeButtons->button(std::clamp(hooks.themePreference(), 0, 2)))
            button->setChecked(true);
    }
    if (hooks.chatTextSize)
    {
        const int index = textSizeCombo->findData(hooks.chatTextSize());
        textSizeCombo->setCurrentIndex(index >= 0 ? index : 1);
    }
    if (backend.readProfileValue)
    {
        if (auto* temperature = findChild<QDoubleSpinBox*>(QStringLiteral("profileTemperatureSpin")); temperature && !temperature->hasFocus())
        {
            const QSignalBlocker blocker(temperature);
            temperature->setValue(backend.readProfileValue("temperature"));
        }
        if (auto* maxTokens = findChild<QSpinBox*>(QStringLiteral("profileMaxTokensSpin")); maxTokens && !maxTokens->hasFocus())
        {
            const QSignalBlocker blocker(maxTokens);
            maxTokens->setValue(static_cast<int>(backend.readProfileValue("maxTokens")));
        }
    }
    updating = false;
    RenderTexts();
    EvaluateDependencies();
    UpdateRestartBanner();
    if (!search->text().trimmed().isEmpty())
        ApplySearch(search->text());
}

void SettingsDialog::SetCompanionName(const QString& name)
{
    companionName = name;
    RenderTexts();
    EvaluateDependencies();
}

QString SettingsDialog::Expand(const QString& text) const
{
    QString expanded = WithCompanionName(text, companionName);
    // "This profile ({profile})" reads "This profile" until the backend names the active profile.
    if (profileName.isEmpty())
        expanded.replace(QStringLiteral(" ({profile})"), QString());
    else
        expanded.replace(QStringLiteral("{profile}"), profileName);
    return expanded;
}

QString SettingsDialog::LabelFor(const std::string& key) const
{
    if (Row* row = FindRow(key))
        return Expand(row->descriptor.label);
    if (const SettingDescriptor* descriptor = FindSetting(key))
        return Expand(descriptor->label);
    return QString::fromStdString(key);
}

void SettingsDialog::ShowSection(const SettingsSection id)
{
    const int index = static_cast<int>(id);
    if (index < 0 || index >= stack->count())
        return;
    if (sectionList->item(index)->isHidden())
        search->clear();
    sectionList->setCurrentRow(index);
    stack->setCurrentIndex(index);
}

bool SettingsDialog::ShowSection(const QString& nameOrKey)
{
    const QString wanted = nameOrKey.trimmed();
    for (const SettingsSection id : SettingsSections())
    {
        if (wanted.compare(SectionName(id), Qt::CaseInsensitive) == 0 || wanted.compare(QString::fromUtf8(SectionId(id)), Qt::CaseInsensitive) == 0)
        {
            ShowSection(id);
            return true;
        }
    }
    const std::string key = wanted.toStdString();
    if (FindRow(key) != nullptr || FindSetting(key) != nullptr)
    {
        FocusSetting(key);
        return true;
    }
    return false;
}

void SettingsDialog::FocusSetting(const std::string& keyOrObjectName)
{
    QWidget* target = nullptr;
    QCheckBox* focus = nullptr;
    SettingsSection sectionId = SettingsSection::General;
    if (Row* row = FindRow(keyOrObjectName))
    {
        target = row->frame;
        focus = row->toggle;
        sectionId = row->group->section;
    }
    else if (const SettingDescriptor* descriptor = FindSetting(keyOrObjectName))
    {
        sectionId = descriptor->section;
        focus = Switch(keyOrObjectName);
        target = focus != nullptr ? focus->parentWidget() : nullptr;
    }
    else
    {
        return;
    }
    if (!search->text().isEmpty())
        search->clear();
    ShowSection(sectionId);
    if (!isVisible())
        show();
    raise();
    activateWindow();
    if (target == nullptr)
        return;
    QScrollArea* page = SectionFor(sectionId).page;
    QPointer<QWidget> flashed(target);
    // Scrolled now, and once more after the page that was just shown has been laid out.
    const auto reveal = [page, flashed]()
    {
        if (flashed)
            page->ensureWidgetVisible(flashed, 0, 48);
    };
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    reveal();
    QTimer::singleShot(0, this, reveal);
    if (focus != nullptr && focus->isEnabled())
        focus->setFocus(Qt::OtherFocusReason);
    target->setProperty("highlight", true);
    Repolish(target);
    QTimer::singleShot(HighlightMilliseconds, this, [flashed]()
    {
        if (!flashed)
            return;
        flashed->setProperty("highlight", false);
        Repolish(flashed);
    });
}

QWidget* SettingsDialog::SectionPage(const SettingsSection id) const
{
    const Section* section = SectionFor(id);
    return section != nullptr ? section->page : nullptr;
}

QList<QWidget*> SettingsDialog::SectionPages() const
{
    QList<QWidget*> pages;
    for (const auto& section : sections)
        pages.push_back(section->page);
    return pages;
}

QWidget* SettingsDialog::AddCustomRow(const SettingsSection section, const QString& group, const QString& label,
    const QString& description, QWidget* control, const QString& rowObjectName, std::vector<std::string> requiresAll)
{
    Row& row = AddPlainRow(GroupFor(section, group), label, description, control, rowObjectName, std::move(requiresAll));
    EvaluateDependencies();
    if (!search->text().trimmed().isEmpty())
        ApplySearch(search->text());
    return row.frame;
}

QWidget* SettingsDialog::AddLinkRow(const SettingsSection section, const QString& group, const QString& label,
    const QString& description, const QString& buttonText, std::function<void()> onClick, const QString& rowObjectName)
{
    auto* button = new QPushButton(buttonText);
    button->setAutoDefault(false);
    button->setAccessibleName(buttonText);
    connect(button, &QPushButton::clicked, this, [onClick = std::move(onClick)]()
    {
        if (onClick)
            onClick();
    });
    return AddCustomRow(section, group, label, description, button, rowObjectName);
}

void SettingsDialog::SetHostedWidget(const SettingsSection sectionId, const QString& slot, QWidget* widget, const int position)
{
    Section& section = SectionFor(sectionId);
    QPointer<QWidget>& current = section.hosted[slot];
    if (current != nullptr && current != widget)
    {
        section.body->removeWidget(current);
        current->hide();
        current->setParent(nullptr);
    }
    current = widget;
    if (widget == nullptr)
        return;
    widget->setParent(section.content);
    const int count = section.body->count();
    section.body->insertWidget(position < 0 || position > count ? count : position, widget);
    // A widget taken out of another layout or a tab page may have been hidden there.
    widget->setVisible(true);
    EvaluateDependencies();
    if (!search->text().trimmed().isEmpty())
        ApplySearch(search->text());
}

void SettingsDialog::SetLocked(const bool locked)
{
    setEnabled(!locked);
}

void SettingsDialog::SetResizeHook(std::function<void(QSize)> hook)
{
    resizeHook = std::move(hook);
    NotifyResize();
}

void SettingsDialog::NotifyResize()
{
    if (resizeHook)
        resizeHook(stack->size());
}

int SettingsDialog::HostedHits(const Section& section, const QStringList& terms) const
{
    int labelHits = 0;
    int catalogHits = 0;
    for (const auto& [slot, widget] : section.hosted)
    {
        if (widget == nullptr)
            continue;
        // What the owner can read in the hosted panel, walked now because the panel may have
        // been rebuilt since the last query.
        for (const QLabel* label : widget->findChildren<QLabel*>())
        {
            if (!label->text().isEmpty() && Matches(label->text(), terms))
                ++labelHits;
        }
        for (const QAbstractButton* button : widget->findChildren<QAbstractButton*>())
        {
            if (!button->text().isEmpty() && Matches(button->text(), terms))
                ++labelHits;
        }
        for (const SettingDescriptor& descriptor : SettingCatalog())
        {
            if (descriptor.host == SettingHost::Dialog || descriptor.section != section.id ||
                widget->findChild<QCheckBox*>(QString::fromStdString(descriptor.objectName)) == nullptr)
                continue;
            const QString haystack = Expand(descriptor.label) + QLatin1Char(' ') + Expand(descriptor.description) + QLatin1Char(' ') +
                                     descriptor.keywords.join(QLatin1Char(' '));
            if (Matches(haystack, terms))
                ++catalogHits;
        }
    }
    return std::max(labelHits, catalogHits);
}

void SettingsDialog::ApplySearch(const QString& query)
{
    const QStringList terms = query.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (terms.isEmpty())
    {
        ShowAllRows();
        return;
    }
    int total = 0;
    int firstWithHits = -1;
    for (const auto& section : sections)
    {
        int hits = 0;
        for (const auto& group : groups)
        {
            if (group->section != section->id)
                continue;
            for (Row* row : group->rows)
            {
                const QString haystack = Expand(row->descriptor.label) + QLatin1Char(' ') + Expand(row->descriptor.description) +
                                         QLatin1Char(' ') + row->descriptor.keywords.join(QLatin1Char(' '));
                row->matches = Matches(haystack, terms);
                hits += row->matches ? 1 : 0;
            }
            LayoutDividers(*group);
        }
        hits += HostedHits(*section, terms);
        section->item->setData(HitCountRole, hits > 0 ? QString::number(hits) : QString());
        section->item->setData(Qt::AccessibleTextRole,
            QStringLiteral("%1, %2 match%3").arg(SectionName(section->id)).arg(hits).arg(hits == 1 ? "" : "es"));
        section->item->setHidden(hits == 0);
        if (hits > 0 && firstWithHits < 0)
            firstWithHits = static_cast<int>(section->id);
        total += hits;
    }
    noResults->setText(QStringLiteral("No settings match \xE2\x80\x9C%1\xE2\x80\x9D").arg(query.trimmed()));
    noResults->setVisible(total == 0);
    stack->setVisible(total > 0);
    const int current = stack->currentIndex();
    if (firstWithHits >= 0 && (current < 0 || sectionList->item(current)->isHidden()))
    {
        sectionList->setCurrentRow(firstWithHits);
        stack->setCurrentIndex(firstWithHits);
    }
}

void SettingsDialog::ShowAllRows()
{
    for (const auto& row : rows)
        row->matches = true;
    for (const auto& group : groups)
        LayoutDividers(*group);
    for (const auto& section : sections)
    {
        section->item->setData(HitCountRole, QString());
        section->item->setData(Qt::AccessibleTextRole, SectionName(section->id));
        section->item->setHidden(false);
    }
    noResults->hide();
    stack->show();
}

void SettingsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    Refresh();
    search->setFocus(Qt::OtherFocusReason);
    NotifyResize();
}

void SettingsDialog::hideEvent(QHideEvent* event)
{
    QSettings().setValue(GeometryKey, saveGeometry());
    if (hooks.onHide)
        hooks.onHide();
    QDialog::hideEvent(event);
}

void SettingsDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    // Icons are drawn in the palette's colours, so a theme change needs them fetched again.
    if (event->type() == QEvent::PaletteChange)
        ApplyIcons();
}

void SettingsDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    navigation->setFixedWidth(width() < 900 ? 168 : 200);
    NotifyResize();
}

bool SettingsDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == search && event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Down)
    {
        // Down from the search box continues into the section list.
        sectionList->setFocus(Qt::TabFocusReason);
        return true;
    }
    if (event->type() == QEvent::MouseButtonRelease)
    {
        const auto* click = static_cast<QMouseEvent*>(event);
        Row* row = FindRowByWidget(watched);
        if (row != nullptr && row->toggle != nullptr && click->button() == Qt::LeftButton && row->frame->rect().contains(click->pos()) &&
            row->toggle->isEnabled())
        {
            // The whole row is the switch's hit area, the way CapabilityPanel rows work.
            row->toggle->toggle();
            return true;
        }
    }
    if (event->type() == QEvent::ContextMenu)
    {
        Row* row = FindRowByWidget(watched);
        if (row != nullptr && row->hasSwitch &&
            (row->descriptor.store == SettingStore::Preference || row->descriptor.store == SettingStore::SettingsFile))
        {
            QMenu menu(this);
            QAction* reset = menu.addAction(QStringLiteral("Reset to default"));
            reset->setEnabled(row->frame->isEnabled() && !row->busy && static_cast<bool>(backend.reset));
            const std::string key = row->descriptor.key;
            if (menu.exec(static_cast<QContextMenuEvent*>(event)->globalPos()) == reset)
                ResetRow(key);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void SettingsDialog::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && !search->text().isEmpty())
    {
        // Esc clears the search first and closes the dialog only once it is empty.
        search->clear();
        search->setFocus(Qt::ShortcutFocusReason);
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && qobject_cast<QAbstractButton*>(focusWidget()) == nullptr)
    {
        // A QDialog would otherwise press its first button when Enter reaches it from a line edit.
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}
}
