#include "commandPalette.h"

#include "softShadow.h"

#include <QAction>
#include <QApplication>
#include <QFontMetrics>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <utility>

namespace revia::desktop
{
namespace
{
constexpr int IndexRole = Qt::UserRole;    // entry rows: index into CommandPalette::visible
constexpr int KindRole = Qt::UserRole + 1; // EntryRow or HeaderRow
constexpr int SubtitleRole = Qt::UserRole + 2;
constexpr int ShortcutRole = Qt::UserRole + 3;
constexpr int EntryRow = 0;
constexpr int HeaderRow = 1;
constexpr int RowHeight = 40;
constexpr int HeaderHeight = 28;
constexpr int VisibleRows = 8;
constexpr int MaximumDynamic = 8;
constexpr int MaximumRecent = 6;

const QString& RecentGroup()
{
    static const QString name = QStringLiteral("Recent");
    return name;
}

const QString& ChatsGroup()
{
    static const QString name = QStringLiteral("Chats");
    return name;
}

// Where a group sits when the query is empty (and breaks ties between equally good groups).
int GroupRank(const QString& group, const int firstSeen)
{
    static const QStringList order = {RecentGroup(), "Actions", "Panels", "Settings", ChatsGroup(), "Companions"};
    const qsizetype rank = order.indexOf(group);
    return rank >= 0 ? static_cast<int>(rank) : static_cast<int>(order.size()) + firstSeen;
}

// Draws a palette row from the QPalette alone, so it follows the theme without knowing it: the
// title in Text (HighlightedText when highlighted), the subtitle and shortcut chip in
// PlaceholderText, disabled rows in the disabled Text colour.
class PaletteRowDelegate final : public QStyledItemDelegate
{
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        return {option.rect.width(), index.data(KindRole).toInt() == HeaderRow ? HeaderHeight : RowHeight};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QPalette& palette = option.palette;
        const QRect rect = option.rect;
        if (index.data(KindRole).toInt() == HeaderRow)
        {
            QFont font = option.font;
            font.setPixelSize(12);
            font.setWeight(QFont::DemiBold);
            painter->setFont(font);
            painter->setPen(palette.color(QPalette::PlaceholderText));
            painter->drawText(rect.adjusted(14, 0, -14, -5), Qt::AlignLeft | Qt::AlignBottom, index.data(Qt::DisplayRole).toString());
            painter->restore();
            return;
        }
        const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        const QRect band = rect.adjusted(4, 1, -4, -1);
        if (selected || (hovered && enabled))
        {
            painter->setPen(Qt::NoPen);
            painter->setBrush(palette.color(selected ? QPalette::Highlight : QPalette::AlternateBase));
            painter->drawRoundedRect(band, 6, 6);
        }
        const QColor disabled = palette.color(QPalette::Disabled, QPalette::Text);
        const QColor primary = !enabled ? disabled : palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        const QColor secondary = !enabled ? disabled : palette.color(QPalette::PlaceholderText);

        int x = band.left() + 10;
        const int right = band.right() - 10;
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        if (!icon.isNull())
        {
            const QIcon::Mode mode = !enabled ? QIcon::Disabled : selected ? QIcon::Selected : QIcon::Normal;
            icon.paint(painter, QRect(x, rect.center().y() - 9, 18, 18), Qt::AlignCenter, mode);
        }
        x += 28; // the icon column stays even when a row has none, so titles line up

        int textRight = right;
        const QString shortcut = index.data(ShortcutRole).toString();
        if (!shortcut.isEmpty())
        {
            QFont chipFont = option.font;
            chipFont.setPixelSize(11);
            chipFont.setWeight(QFont::DemiBold);
            const int chipWidth = QFontMetrics(chipFont).horizontalAdvance(shortcut) + 14;
            const int chipHeight = 20;
            const QRect chip(right - chipWidth + 1, rect.center().y() - chipHeight / 2, chipWidth, chipHeight);
            QColor outline = secondary;
            outline.setAlpha(90);
            painter->setPen(QPen(outline, 1));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(chip).adjusted(0.5, 0.5, -0.5, -0.5), chipHeight / 2.0, chipHeight / 2.0);
            painter->setFont(chipFont);
            painter->setPen(secondary);
            painter->drawText(chip, Qt::AlignCenter, shortcut);
            textRight = chip.left() - 12;
        }

        QFont titleFont = option.font;
        titleFont.setPixelSize(13);
        const QFontMetrics titleMetrics(titleFont);
        const int room = std::max(0, textRight - x);
        const QString title = titleMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, room);
        const int titleWidth = titleMetrics.horizontalAdvance(title);
        painter->setFont(titleFont);
        painter->setPen(primary);
        painter->drawText(QRect(x, rect.top(), titleWidth + 1, rect.height()), Qt::AlignLeft | Qt::AlignVCenter, title);

        const QString subtitle = index.data(SubtitleRole).toString();
        const int subtitleRoom = room - titleWidth - 8;
        if (!subtitle.isEmpty() && subtitleRoom > 24)
        {
            QFont subtitleFont = option.font;
            subtitleFont.setPixelSize(12);
            painter->setFont(subtitleFont);
            painter->setPen(secondary);
            painter->drawText(QRect(x + titleWidth + 8, rect.top(), subtitleRoom, rect.height()), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(subtitleFont).elidedText(subtitle, Qt::ElideRight, subtitleRoom));
        }
        painter->restore();
    }
};
}

int FuzzyScore(const QString& query, const QString& candidate)
{
    const QString needle = query.trimmed().toCaseFolded();
    if (needle.isEmpty())
        return 0;
    const QString haystack = candidate.toCaseFolded();
    if (haystack.startsWith(needle))
        return 1000;
    bool contiguous = false;
    for (qsizetype at = haystack.indexOf(needle); at >= 0; at = haystack.indexOf(needle, at + 1))
    {
        // at > 0: a match at the start was the prefix case above.
        if (!haystack.at(at - 1).isLetterOrNumber())
            return 600;
        contiguous = true;
    }
    if (contiguous)
        return 300;
    qsizetype first = -1;
    qsizetype last = -1;
    qsizetype from = 0;
    for (const QChar character : needle)
    {
        const qsizetype at = haystack.indexOf(character, from);
        if (at < 0)
            return -1;
        if (first < 0)
            first = at;
        last = at;
        from = at + 1;
    }
    const qsizetype gaps = (last - first + 1) - needle.size();
    return std::max(1, 100 - static_cast<int>(gaps));
}

CommandPalette::CommandPalette(QWidget* hostWidget) : QWidget(hostWidget), host(hostWidget)
{
    // No WA_StyledBackground: paintEvent draws the stylesheet's scrim itself, under the shadow.
    setObjectName("commandPaletteScrim");
    hide();

    card = new QFrame(this);
    card->setObjectName("commandPalette");
    auto* column = new QVBoxLayout(card);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    input = new QLineEdit(card);
    input->setObjectName("commandPaletteInput");
    input->setPlaceholderText("Search commands, panels, settings and chats");
    input->setAccessibleName("Command palette");
    input->installEventFilter(this);
    connect(input, &QLineEdit::textChanged, this,
        [this]()
        {
            Rebuild(false);
            if (dynamicSource)
                dynamicDelay->start();
        });
    column->addWidget(input);
    list = new QListWidget(card);
    list->setObjectName("commandPaletteList");
    list->setItemDelegate(new PaletteRowDelegate(list));
    // The input keeps the keyboard; the list follows it (Up, Down, Enter) and takes clicks.
    list->setFocusPolicy(Qt::NoFocus);
    list->setFrameShape(QFrame::NoFrame);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setMouseTracking(true);
    connect(list, &QListWidget::itemClicked, this,
        [this](QListWidgetItem* item)
        {
            const int row = list->row(item);
            if (!IsRunnable(row))
                return;
            list->setCurrentRow(row);
            RunCurrent();
        });
    column->addWidget(list);
    empty = new QLabel("No matches", card);
    empty->setObjectName("commandPaletteEmpty");
    empty->setAlignment(Qt::AlignCenter);
    empty->hide();
    column->addWidget(empty);
    footer = new QLabel("↑↓ to move · Enter to open · Esc to close", card);
    footer->setObjectName("commandPaletteFooter");
    column->addWidget(footer);

    dynamicDelay = new QTimer(this);
    dynamicDelay->setSingleShot(true);
    dynamicDelay->setInterval(150);
    connect(dynamicDelay, &QTimer::timeout, this, [this]() { FlushDynamicSource(); });
    if (host)
        host->installEventFilter(this);
}

void CommandPalette::SetCommands(std::vector<CommandEntry> entries)
{
    commands = std::move(entries);
    if (IsOpen())
        Rebuild(true);
}

void CommandPalette::SetDynamicSource(std::function<std::vector<CommandEntry>(const QString&)> source)
{
    dynamicSource = std::move(source);
}

void CommandPalette::SetAvailable(std::function<bool()> check)
{
    available = std::move(check);
}

void CommandPalette::Open(const QString& query)
{
    OpenWith(query, false);
}

void CommandPalette::OpenChats()
{
    OpenWith({}, true);
}

void CommandPalette::OpenWith(const QString& query, const bool chats)
{
    if (!host || (available && !available()))
        return;
    if (!IsOpen())
        previousFocus = QApplication::focusWidget();
    chatsOnly = chats;
    input->setPlaceholderText(chats ? QStringLiteral("Search chats") : QStringLiteral("Search commands, panels, settings and chats"));
    {
        const QSignalBlocker blocker(input);
        input->setText(query);
    }
    setGeometry(host->rect());
    show();
    raise();
    input->setFocus(Qt::ShortcutFocusReason);
    // Chats are fetched at once on opening, so the list is complete from the first frame.
    dynamicDelay->stop();
    FetchDynamic();
    Rebuild(false);
}

void CommandPalette::Close()
{
    if (!isVisible())
        return;
    dynamicDelay->stop();
    hide();
    if (previousFocus && previousFocus->isVisible() && previousFocus->isEnabled())
        previousFocus->setFocus(Qt::OtherFocusReason);
    previousFocus = nullptr;
}

bool CommandPalette::IsOpen() const
{
    return isVisible();
}

void CommandPalette::SetIconProvider(std::function<QIcon(const char*)> provider)
{
    iconProvider = std::move(provider);
    if (searchIcon == nullptr && iconProvider)
        searchIcon = input->addAction(QIcon(), QLineEdit::LeadingPosition);
    if (searchIcon != nullptr)
        searchIcon->setIcon(iconProvider ? iconProvider("search") : QIcon());
}

QString CommandPalette::CurrentId() const
{
    const int row = list->currentRow();
    const QListWidgetItem* item = list->item(row);
    if (item == nullptr || item->data(KindRole).toInt() != EntryRow)
        return {};
    return visible.at(static_cast<std::size_t>(item->data(IndexRole).toInt())).id;
}

bool CommandPalette::IsRunnable(const int row) const
{
    const QListWidgetItem* item = list->item(row);
    return item != nullptr && item->data(KindRole).toInt() == EntryRow && item->flags().testFlag(Qt::ItemIsEnabled);
}

bool CommandPalette::RunCurrent()
{
    const int row = list->currentRow();
    if (!IsRunnable(row))
        return false;
    const CommandEntry& entry = visible.at(static_cast<std::size_t>(list->item(row)->data(IndexRole).toInt()));
    if (entry.enabled && !entry.enabled())
        return false;
    // Copies: running may replace the commands (a companion switch rebuilds them).
    const std::function<void()> run = entry.run;
    const QString id = entry.id;
    Close();
    Remember(id);
    if (run)
        run();
    return true;
}

void CommandPalette::FlushDynamicSource()
{
    dynamicDelay->stop();
    if (!dynamicSource || !IsOpen())
        return;
    FetchDynamic();
    Rebuild(true);
}

void CommandPalette::FetchDynamic()
{
    dynamicEntries.clear();
    dynamicQuery = input->text().trimmed();
    if (!dynamicSource)
        return;
    std::vector<CommandEntry> found = dynamicSource(dynamicQuery);
    if (found.size() > static_cast<std::size_t>(MaximumDynamic))
        found.resize(static_cast<std::size_t>(MaximumDynamic));
    for (CommandEntry& entry : found)
    {
        if (entry.group.isEmpty())
            entry.group = ChatsGroup();
    }
    dynamicEntries = std::move(found);
}

void CommandPalette::Remember(const QString& id) const
{
    if (id.isEmpty())
        return;
    QSettings settings;
    QStringList recent = settings.value("palette/recent").toStringList();
    recent.removeAll(id);
    recent.prepend(id);
    while (recent.size() > MaximumRecent)
        recent.removeLast();
    settings.setValue("palette/recent", recent);
}

void CommandPalette::Rebuild(const bool keepSelection)
{
    const QString keep = keepSelection ? CurrentId() : QString();
    const QString query = input->text().trimmed();
    struct Scored
    {
        int score;
        int order;
        const CommandEntry* entry;
    };
    struct Group
    {
        QString name;
        int rank;
        std::vector<Scored> entries;
    };
    std::vector<Group> groups;
    const auto groupNamed = [&groups](const QString& name) -> Group&
    {
        for (Group& group : groups)
        {
            if (group.name == name)
                return group;
        }
        groups.push_back({name, GroupRank(name, static_cast<int>(groups.size())), {}});
        return groups.back();
    };

    std::vector<const CommandEntry*> recent;
    if (query.isEmpty() && !chatsOnly)
    {
        const QStringList ids = QSettings().value("palette/recent").toStringList();
        for (const QString& id : ids)
        {
            for (const std::vector<CommandEntry>* source : {&commands, &dynamicEntries})
            {
                const auto found =
                    std::find_if(source->begin(), source->end(), [&id](const CommandEntry& entry) { return entry.id == id; });
                if (found != source->end() && std::find(recent.begin(), recent.end(), &*found) == recent.end())
                {
                    recent.push_back(&*found);
                    break;
                }
            }
        }
        for (std::size_t index = 0; index < recent.size(); ++index)
            groupNamed(RecentGroup()).entries.push_back({0, static_cast<int>(index), recent[index]});
    }

    int order = 0;
    const auto consider = [&](const CommandEntry& entry, const bool dynamic)
    {
        ++order;
        const QString group = entry.group.isEmpty() ? QStringLiteral("Actions") : entry.group;
        if (chatsOnly && group != ChatsGroup())
            return;
        // Listed once: under Recent when it is there.
        if (std::find(recent.begin(), recent.end(), &entry) != recent.end())
            return;
        int score = entry.priority;
        if (!query.isEmpty())
        {
            int match = FuzzyScore(query, entry.title);
            for (const QString& keyword : entry.keywords)
                match = std::max(match, FuzzyScore(query, keyword));
            // The source matched it for this query, perhaps on text the title does not show. Results
            // for an older query, still waiting for the debounce, must match on their titles.
            if (dynamic && query == dynamicQuery)
                match = std::max(match, 50);
            if (match < 0)
                return;
            score += match;
        }
        groupNamed(group).entries.push_back({score, order, &entry});
    };
    for (const CommandEntry& entry : commands)
        consider(entry, false);
    for (const CommandEntry& entry : dynamicEntries)
        consider(entry, true);

    for (Group& group : groups)
    {
        std::stable_sort(group.entries.begin(), group.entries.end(),
            [](const Scored& first, const Scored& second) { return first.score > second.score; });
    }
    const auto best = [](const Group& group) { return group.entries.empty() ? -1 : group.entries.front().score; };
    std::stable_sort(groups.begin(), groups.end(),
        [&](const Group& first, const Group& second)
        {
            if (!query.isEmpty() && best(first) != best(second))
                return best(first) > best(second);
            return first.rank < second.rank;
        });

    list->clear();
    visible.clear();
    for (const Group& group : groups)
    {
        if (group.entries.empty())
            continue;
        auto* header = new QListWidgetItem(group.name, list);
        header->setFlags(Qt::NoItemFlags);
        header->setData(KindRole, HeaderRow);
        for (const Scored& scored : group.entries)
        {
            const CommandEntry& entry = *scored.entry;
            visible.push_back(entry);
            auto* item = new QListWidgetItem(entry.icon, entry.title, list);
            item->setData(KindRole, EntryRow);
            item->setData(IndexRole, static_cast<int>(visible.size() - 1));
            item->setData(SubtitleRole, entry.subtitle);
            item->setData(ShortcutRole, entry.shortcutText);
            QString spoken = entry.title;
            if (!entry.subtitle.isEmpty())
                spoken += QStringLiteral(", ") + entry.subtitle;
            if (!entry.shortcutText.isEmpty())
                spoken += QStringLiteral(", ") + entry.shortcutText;
            item->setData(Qt::AccessibleTextRole, spoken);
            item->setData(Qt::AccessibleDescriptionRole, entry.subtitle);
            const bool enabled = !entry.enabled || entry.enabled();
            item->setFlags(enabled ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags);
        }
    }

    int selected = -1;
    for (int row = 0; row < list->count(); ++row)
    {
        if (!IsRunnable(row))
            continue;
        if (selected < 0)
            selected = row;
        if (!keep.isEmpty() && visible.at(static_cast<std::size_t>(list->item(row)->data(IndexRole).toInt())).id == keep)
        {
            selected = row;
            break;
        }
    }
    list->setCurrentRow(selected);
    if (selected >= 0)
        list->scrollToItem(list->item(selected));
    if (selected <= 1)
        list->scrollToTop();
    list->setVisible(list->count() > 0);
    empty->setVisible(list->count() == 0);
    LayoutCard();
}

void CommandPalette::Move(const int step)
{
    const int row = list->currentRow();
    const int direction = step < 0 ? -1 : 1;
    int remaining = std::abs(step);
    int target = row;
    for (int probe = row + direction; probe >= 0 && probe < list->count() && remaining > 0; probe += direction)
    {
        // Headers and disabled entries are passed over.
        if (IsRunnable(probe))
        {
            target = probe;
            --remaining;
        }
    }
    if (target == row || target < 0)
        return;
    list->setCurrentRow(target);
    list->scrollToItem(list->item(target));
    // Moving onto the first entry also brings its group header back into view.
    bool firstEntry = true;
    for (int probe = target - 1; probe >= 0 && firstEntry; --probe)
        firstEntry = !IsRunnable(probe);
    if (firstEntry)
        list->scrollToTop();
}

void CommandPalette::LayoutCard()
{
    if (!host)
        return;
    const int width = std::max(240, std::min(640, host->width() - 48));
    const int top = std::max(56, host->height() * 15 / 100);
    int rows = 0;
    for (int row = 0; row < list->count(); ++row)
        rows += list->item(row)->data(KindRole).toInt() == HeaderRow ? HeaderHeight : RowHeight;
    const int chrome = input->sizeHint().height() + footer->sizeHint().height();
    const int room = std::max(2 * RowHeight, host->height() - top - 24 - chrome);
    list->setFixedHeight(std::min({rows + 8, VisibleRows * RowHeight + 8, room}));
    card->setFixedWidth(width);
    card->adjustSize();
    card->move((this->width() - width) / 2, top);
    update();
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == host && event->type() == QEvent::Resize && isVisible())
    {
        setGeometry(host->rect());
        LayoutCard();
    }
    if (watched == input && event->type() == QEvent::KeyPress)
    {
        switch (static_cast<QKeyEvent*>(event)->key())
        {
        case Qt::Key_Down:
        case Qt::Key_Tab: // Tab never leaves the palette
            Move(1);
            return true;
        case Qt::Key_Up:
        case Qt::Key_Backtab:
            Move(-1);
            return true;
        case Qt::Key_PageDown:
            Move(VisibleRows);
            return true;
        case Qt::Key_PageUp:
            Move(-VisibleRows);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            RunCurrent();
            return true;
        case Qt::Key_Escape:
            Close();
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CommandPalette::mousePressEvent(QMouseEvent* event)
{
    if (!card->geometry().contains(event->position().toPoint()))
        Close();
    event->accept();
}

void CommandPalette::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    QStyleOption option;
    option.initFrom(this);
    style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);
    QColor shadow = palette().color(QPalette::Shadow);
    // As in SoftShadowHost: an opaque Shadow is Fusion's bevel colour, not a theme's drop shadow.
    if (shadow.alpha() == 255)
        shadow.setAlpha(48);
    PaintSoftShadow(painter, card->geometry(), 14, 24, QPoint(0, 8), shadow);
}

void CommandPalette::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) && searchIcon != nullptr && iconProvider)
        searchIcon->setIcon(iconProvider("search"));
}
}
