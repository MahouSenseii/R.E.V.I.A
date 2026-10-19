#include "panelWindow.h"

#include "desktopObjectNames.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QLayout>
#include <QLineEdit>
#include <QMargins>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QStringList>
#include <QTextDocument>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <QWindow>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <utility>

namespace revia::desktop
{
namespace
{
// SettingsSection values from settingsTypes.h, spelled out so the shell widgets do not include the
// settings headers. Revia.ShellWidgets checks them against the enum.
constexpr int NoSection = -1;
constexpr int ChatSection = 2;
constexpr int VoiceSection = 3;
constexpr int VisionSection = 4;
constexpr int InternetSection = 5;
constexpr int MemorySection = 6;
constexpr int IntegrationsSection = 9;
constexpr int AdvancedSection = 10;
constexpr const char* SectionNames[] = {
    "General", "Appearance", "Chat", "Voice", "Vision", "Internet", "Memory", "Models", "Permissions", "Integrations", "Advanced"};

constexpr int PanelGap = 12;
constexpr int CascadeStep = 32;
constexpr int MinimumOwnerWidth = 760;

QString FromView(const std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString GeometryKey(const PanelSpec& spec)
{
    return QStringLiteral("panels/%1/geometry").arg(QString::fromLatin1(spec.key));
}

// Esc belongs to a text field the owner is typing in; anywhere else it closes the window.
bool EditingText(QWidget* widget)
{
    if (const auto* line = qobject_cast<QLineEdit*>(widget))
        return !line->isReadOnly() && !line->text().isEmpty();
    if (const auto* plain = qobject_cast<QPlainTextEdit*>(widget))
        return !plain->isReadOnly() && !plain->document()->isEmpty();
    if (const auto* rich = qobject_cast<QTextEdit*>(widget))
        return !rich->isReadOnly() && !rich->document()->isEmpty();
    if (const auto* combo = qobject_cast<QComboBox*>(widget))
        return combo->isEditable() && !combo->currentText().isEmpty();
    return false;
}

QToolButton* HeaderButton(QWidget* parent, const char* name, const QString& tip)
{
    auto* button = new QToolButton(parent);
    button->setObjectName(name);
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setText(tip); // shown only until an icon arrives
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::TabFocus);
    button->setFixedSize(32, 32);
    button->setIconSize(QSize(18, 18));
    return button;
}
}

const std::vector<PanelSpec>& PanelCatalog()
{
    // In PanelId order: Spec() indexes by the enum value.
    static const std::vector<PanelSpec> catalog{
        {PanelId::Canvas, "canvas", "Canvas", "Workspace", "image", "canvasPage", {}, {640, 560}, 480, NoSection, nullptr},
        {PanelId::Vision, "vision", "Vision", "Workspace", "eye", "visionTab", {}, {600, 620}, 480, VisionSection, "sectionTitle"},
        {PanelId::Profiles, "profiles", "Profiles", "Companion", "user", "profilesTab", {}, {640, 680}, 520, ChatSection, "sectionTitle"},
        {PanelId::Mind, "mind", "Mind", "Companion", "brain", "mindTab", {}, {600, 600}, 480, NoSection, "sectionTitle"},
        {PanelId::Memory, "memory", "Memory", "Companion", "database", "memoryTab", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M),
            {720, 640}, 560, MemorySection, "sectionTitle"},
        {PanelId::Voice, "voice", "Voice", "Companion", "waveform", "voiceTab", {}, {600, 700}, 480, VoiceSection, nullptr},
        {PanelId::Presence, "presence", "Presence", "Companion", "sparkle", "presencePage", {}, {560, 560}, 480, IntegrationsSection,
            nullptr},
        {PanelId::Audience, "audience", "Audience", "Companion", "users", "audienceStudioScroll", {}, {600, 680}, 480, NoSection,
            "sectionTitle"},
        {PanelId::Agents, "agents", "Agents", "Studio", "bot", "agentStudioScroll", {}, {720, 700}, 560, NoSection, "sectionTitle"},
        {PanelId::Learning, "learning", "Learning", "Studio", "graduation", "learningStudioScroll", {}, {640, 660}, 520, NoSection,
            "sectionTitle"},
        {PanelId::Development, "development", "Development", "Studio", "code", "developmentStudioScroll", {}, {600, 600}, 480, NoSection,
            "sectionTitle"},
        {PanelId::Activity, "activity", "Activity", "Runtime", "activity", "activityPage", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A),
            {640, 600}, 480, NoSection, nullptr},
        {PanelId::Pipelines, "pipelines", "Pipelines", "Runtime", "git-branch", "pipelinePage", {}, {600, 520}, 480, NoSection, nullptr},
        {PanelId::InternetActivity, "internet", "Internet activity", "Runtime", "globe", "internetActivityPage", {}, {640, 560}, 480,
            InternetSection, nullptr},
        {PanelId::Resources, "resources", "Resources", "Runtime", "cpu", "resourcePage", {}, {680, 640}, 520, AdvancedSection, nullptr},
    };
    return catalog;
}

const PanelSpec& Spec(const PanelId id)
{
    return PanelCatalog().at(static_cast<std::size_t>(id));
}

std::optional<PanelId> PanelFromKey(const QString& keyOrTitle)
{
    const QString wanted = keyOrTitle.trimmed();
    for (const PanelSpec& spec : PanelCatalog())
    {
        if (wanted.compare(QLatin1String(spec.key), Qt::CaseInsensitive) == 0 ||
            wanted.compare(QString::fromUtf8(spec.title), Qt::CaseInsensitive) == 0)
            return spec.id;
    }
    return std::nullopt;
}

QRect PanelPlacement(const QRect& ownerFrame, const QRect& available, const QSize size, const int cascadeIndex)
{
    const int width = std::clamp(size.width(), 1, std::max(1, available.width()));
    const int height = std::clamp(size.height(), 1, std::max(1, available.height()));
    const int availableRight = available.x() + available.width(); // exclusive
    int x = availableRight - width;                               // flush with the right edge
    if (const int right = ownerFrame.x() + ownerFrame.width() + PanelGap; right + width <= availableRight)
        x = right;
    else if (const int left = ownerFrame.x() - PanelGap - width; left >= available.x())
        x = left;
    const int lowest = std::max(available.y(), available.y() + available.height() - height);
    const int y = std::clamp(ownerFrame.y() + CascadeStep * std::max(0, cascadeIndex), available.y(), lowest);
    return {x, y, width, height};
}

std::optional<QRect> FitSavedPanelFrame(const QRect& saved, const QList<QRect>& availableAreas, const QSize minimum)
{
    if (saved.isEmpty())
        return std::nullopt;
    const QRect* best = nullptr;
    qint64 bestOverlap = 0;
    for (const QRect& area : availableAreas)
    {
        const QRect overlap = area.intersected(saved);
        const qint64 size = static_cast<qint64>(overlap.width()) * overlap.height();
        if (!overlap.isEmpty() && size > bestOverlap)
        {
            best = &area;
            bestOverlap = size;
        }
    }
    if (best == nullptr)
        return std::nullopt;
    const int width = std::clamp(saved.width(), minimum.width(), std::max(minimum.width(), best->width()));
    const int height = std::clamp(saved.height(), minimum.height(), std::max(minimum.height(), best->height()));
    // The lowest x and y that keep the far edges on the area; never left of or above it.
    const int x = std::clamp(saved.x(), best->x(), std::max(best->x(), best->x() + best->width() - width));
    const int y = std::clamp(saved.y(), best->y(), std::max(best->y(), best->y() + best->height() - height));
    return QRect(x, y, width, height);
}

BesidePlacement PlaceBesideGeometry(const QRect& ownerFrame, const QRect& available, const int defaultWidth, const int minUsefulWidth)
{
    const int panelWidth = std::min(available.width(), std::max(minUsefulWidth, std::min(defaultWidth, available.width() * 2 / 5)));
    const int ownerWidth = std::max(MinimumOwnerWidth, available.width() - panelWidth - PanelGap);
    BesidePlacement placement;
    placement.ownerFrame = QRect(available.x(), ownerFrame.y(), ownerWidth, ownerFrame.height());
    // With the owner held at its minimum the panel overlaps it rather than leave the screen.
    const int panelX = std::min(available.x() + ownerWidth + PanelGap, available.x() + available.width() - panelWidth);
    placement.panelFrame = QRect(panelX, ownerFrame.y(), panelWidth, ownerFrame.height());
    return placement;
}

PanelWindow::PanelWindow(const PanelSpec& panelSpec, QWidget* owner) : QWidget(owner, Qt::Window), spec(panelSpec)
{
    setObjectName(FromView(names::kPanelWindowPrefix) + QString::fromLatin1(spec.key));
    setProperty(names::kRoleProperty, QStringLiteral("panelWindow"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_QuitOnClose, false);
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumSize(480, 400);
    // The window itself can hold focus, so opening it leaves no focus ring on a header button.
    setFocusPolicy(Qt::ClickFocus);
    resize(spec.defaultSize);
    SetTitleSuffix({});

    body = new QVBoxLayout(this);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    header = new QWidget(this);
    header->setObjectName("panelHeader");
    header->setFixedHeight(36);
    auto* row = new QHBoxLayout(header);
    row->setContentsMargins(12, 4, 8, 0);
    row->setSpacing(4);
    row->addStretch(1);
    placeBeside = HeaderButton(header, "panelPlaceBeside", "Place beside chat");
    connect(placeBeside, &QToolButton::clicked, this,
        [this]()
        {
            if (placeBesideHook)
                placeBesideHook();
        });
    row->addWidget(placeBeside);
    if (spec.settingsSection >= 0 && spec.settingsSection < static_cast<int>(std::size(SectionNames)))
    {
        openSettings = HeaderButton(
            header, "panelOpenSettings", QStringLiteral("Open %1 settings").arg(QString::fromLatin1(SectionNames[spec.settingsSection])));
        connect(openSettings, &QToolButton::clicked, this,
            [this]()
            {
                if (settingsHook)
                    settingsHook();
            });
        row->addWidget(openSettings);
    }
    body->addWidget(header);
}

void PanelWindow::SetPage(QWidget* const newPage)
{
    if (newPage == page)
    {
        PolishPage();
        return;
    }
    if (page)
    {
        body->removeWidget(page);
        page->hide();
    }
    page = newPage;
    if (!page)
        return;
    page->setParent(this);
    body->addWidget(page, 1);
    // Pages taken out of a QTabWidget or a stacked layout were hidden explicitly; without this
    // they would stay hidden when the window shows.
    page->setVisible(true);
    page->setEnabled(header->isEnabled());
    PolishPage();
}

QWidget* PanelWindow::Page() const
{
    return page;
}

void PanelWindow::PolishPage()
{
    if (!page)
        return;
    QLayout* layout = page->layout();
    if (layout == nullptr)
    {
        if (const auto* scroll = qobject_cast<QScrollArea*>(page.data()); scroll != nullptr && scroll->widget() != nullptr)
            layout = scroll->widget()->layout();
    }
    if (layout != nullptr)
    {
        const QMargins margins = layout->contentsMargins();
        layout->setContentsMargins(
            std::max(margins.left(), 20), std::max(margins.top(), 16), std::max(margins.right(), 20), std::max(margins.bottom(), 16));
    }
    if (spec.redundantTitleObjectName != nullptr)
    {
        if (auto* title = page->findChild<QWidget*>(QString::fromLatin1(spec.redundantTitleObjectName)))
            title->hide();
    }
}

void PanelWindow::SetTitleSuffix(const QString& value)
{
    suffix = value;
    setWindowTitle(QString::fromUtf8(spec.title) + suffix + QStringLiteral(" — Revia"));
}

void PanelWindow::Reveal()
{
    if (isMinimized())
        showNormal();
    else
        show();
    raise();
    activateWindow();
}

void PanelWindow::PlaceAt(const QRect& frame)
{
    placed = true;
    // A window that is not showing has no reliable frame margins (none at all before its first
    // show), so the rectangle is applied by the next showEvent.
    if (isVisible())
        ApplyFrame(frame);
    else
        pendingFrame = frame;
}

QSize PanelWindow::FrameExtra() const
{
    return frameGeometry().size() - size();
}

void PanelWindow::ApplyFrame(const QRect& frame)
{
    const QSize extra = FrameExtra();
    // The minimum size (480x400 of client area) can make the window larger than planned. It then
    // keeps the planned right and bottom edges, the ones the placement kept on the screen.
    const QSize client = (frame.size() - extra).expandedTo(minimumSize());
    const QSize actual = client + extra;
    move(frame.x() + std::min(0, frame.width() - actual.width()), frame.y() + std::min(0, frame.height() - actual.height()));
    resize(client);
}

void PanelWindow::SetLocked(const bool locked)
{
    header->setEnabled(!locked);
    if (page)
        page->setEnabled(!locked);
}

void PanelWindow::SetShowHook(std::function<void(QWidget*)> hook)
{
    showHook = std::move(hook);
    if (shownOnce && isVisible() && showHook)
        showHook(this);
}

void PanelWindow::SetResizeHook(std::function<void(QSize)> hook)
{
    resizeHook = std::move(hook);
}

void PanelWindow::SetSettingsHook(std::function<void()> hook)
{
    settingsHook = std::move(hook);
}

void PanelWindow::SetPlaceBesideHook(std::function<void()> hook)
{
    placeBesideHook = std::move(hook);
}

void PanelWindow::SetPlacementHook(std::function<QRect(QSize)> hook)
{
    placementHook = std::move(hook);
}

void PanelWindow::SetVisibilityHook(std::function<void(bool)> hook)
{
    visibilityHook = std::move(hook);
}

void PanelWindow::SetIconProvider(std::function<QIcon(const char*)> provider)
{
    iconProvider = std::move(provider);
    ApplyIcons();
}

void PanelWindow::SetUnowned(const bool value)
{
    unowned = value;
    ApplyUnowned();
}

void PanelWindow::ApplyUnowned()
{
    if (!unowned)
        return;
    winId(); // the native window must exist before its transient parent can be cleared
    if (QWindow* handle = windowHandle())
        handle->setTransientParent(nullptr);
}

void PanelWindow::ApplyIcons()
{
    if (!iconProvider)
        return;
    placeBeside->setIcon(iconProvider("columns"));
    if (openSettings != nullptr)
        openSettings->setIcon(iconProvider("settings"));
}

bool PanelWindow::RestoreGeometry()
{
    QSettings settings;
    const QString key = GeometryKey(spec);
    const QVariant saved = settings.value(key);
    if (!saved.isValid())
        return false;
    // The saved rectangle is the client area; fitting it to a screen needs the frame around it,
    // which is known here, in the first showEvent.
    const QRect client = saved.toRect();
    const QRect frameNow = frameGeometry();
    const QRect clientNow = geometry();
    const QMargins margins(clientNow.left() - frameNow.left(), clientNow.top() - frameNow.top(), frameNow.right() - clientNow.right(),
        frameNow.bottom() - clientNow.bottom());
    QList<QRect> areas;
    for (const QScreen* screen : QGuiApplication::screens())
        areas.push_back(screen->availableGeometry());
    // A monitor layout that changed since (a smaller screen, a monitor moved) can leave the saved
    // window barely on a screen or larger than it; it comes back whole on the screen it overlaps.
    const std::optional<QRect> frame =
        client.isValid() ? FitSavedPanelFrame(client.marginsAdded(margins), areas, minimumSize() + FrameExtra()) : std::nullopt;
    if (!frame)
    {
        // A monitor that is gone, or a value that is not a rectangle: place it afresh.
        settings.remove(key);
        return false;
    }
    ApplyFrame(*frame);
    return true;
}

void PanelWindow::SaveGeometry() const
{
    QSettings().setValue(GeometryKey(spec), isMaximized() || isFullScreen() ? normalGeometry() : geometry());
}

void PanelWindow::showEvent(QShowEvent* event)
{
    // A spontaneous show is the system restoring a minimised window; it changes nothing here.
    if (!event->spontaneous())
    {
        // The show event arrives before the native window maps, so placing here does not flash,
        // and it is the first point at which the frame margins are known: the native window
        // exists and the widget already counts as visible.
        std::optional<QRect> frame = std::exchange(pendingFrame, std::nullopt);
        if (!placed)
        {
            placed = true;
            if (!RestoreGeometry() && placementHook)
                frame = placementHook(size() + FrameExtra());
        }
        if (frame)
            ApplyFrame(*frame);
        ApplyUnowned();
        PolishPage();
        // Activation would otherwise focus the first button in the tab chain, Place beside chat,
        // and draw its ring. Focus rests on the window (Esc closes, Tab reaches the header and the
        // page) unless the owner already focused something in the page.
        QWidget* focus = focusWidget();
        if (focus == nullptr || focus == placeBeside || focus == openSettings)
            setFocus(Qt::OtherFocusReason);
    }
    QWidget::showEvent(event);
    if (event->spontaneous())
        return;
    if (!shownOnce)
    {
        shownOnce = true;
        if (showHook)
            showHook(this);
    }
    if (resizeHook)
        resizeHook(size());
    if (visibilityHook)
        visibilityHook(true);
}

void PanelWindow::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    if (event->spontaneous())
        return;
    SaveGeometry();
    if (visibilityHook)
        visibilityHook(false);
}

void PanelWindow::closeEvent(QCloseEvent* event)
{
    // Closing only hides: WA_DeleteOnClose is off, so the page and its event-driven widgets live on.
    event->accept();
}

void PanelWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (resizeHook && isVisible())
        resizeHook(event->size());
}

void PanelWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->matches(QKeySequence::Close) || (event->key() == Qt::Key_W && event->modifiers() == Qt::ControlModifier))
    {
        close();
        return;
    }
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier)
    {
        // Keys reach the window only after the focused widget passed on them, so a field with text
        // in it keeps its Esc and the window stays open.
        if (EditingText(focusWidget()))
            event->ignore();
        else
            close();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PanelWindow::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        ApplyIcons();
}

PanelHost::PanelHost(QWidget* const ownerWidget) : owner(ownerWidget)
{
    for (const PanelSpec& spec : PanelCatalog())
    {
        auto* action = new QAction(QString::fromUtf8(spec.title), owner);
        action->setCheckable(true);
        if (!spec.shortcut.isEmpty())
        {
            action->setShortcut(spec.shortcut);
            action->setShortcutContext(Qt::ApplicationShortcut);
        }
        // On the owner as well as in any menu, so the shortcut works while the menu is closed.
        owner->addAction(action);
        const PanelId id = spec.id;
        connections.push_back(QObject::connect(action, &QAction::triggered, owner, [this, id]() { Toggle(id); }));
        actions[id] = action;
    }
    // The owner's focus widget, remembered so it gets focus back when a panel closes.
    connections.push_back(QObject::connect(static_cast<QApplication*>(QCoreApplication::instance()), &QApplication::focusChanged, owner,
        [this](QWidget*, QWidget* now)
        {
            if (now != nullptr && now->window() == owner)
                lastOwnerFocus = now;
        }));
}

PanelHost::~PanelHost()
{
    // The windows outlive the host (they are the owner's children), so nothing they call may
    // reach it any more.
    for (const QMetaObject::Connection& connection : connections)
        QObject::disconnect(connection);
    for (auto& [id, window] : windows)
    {
        if (!window)
            continue;
        window->SetSettingsHook({});
        window->SetPlaceBesideHook({});
        window->SetPlacementHook({});
        window->SetVisibilityHook({});
    }
    for (auto& [id, action] : actions)
        delete action.data();
}

void PanelHost::Adopt(const PanelId id, QWidget* const page)
{
    PanelWindow* window = Window(id);
    if (window == nullptr)
    {
        window = new PanelWindow(Spec(id), owner);
        windows[id] = window;
        Configure(window);
    }
    window->SetPage(page);
}

void PanelHost::Configure(PanelWindow* const window)
{
    const PanelId id = window->Specification().id;
    window->SetLocked(locked);
    window->SetUnowned(unowned);
    if (iconProvider)
        window->SetIconProvider(iconProvider);
    if (showHook)
        window->SetShowHook(showHook);
    if (const auto hook = resizeHooks.find(id); hook != resizeHooks.end())
        window->SetResizeHook(hook->second);
    if (const auto suffix = suffixes.find(id); suffix != suffixes.end())
        window->SetTitleSuffix(suffix->second);
    window->SetSettingsHook(
        [this, id]()
        {
            if (settingsHook)
                settingsHook(Spec(id).settingsSection);
        });
    window->SetPlaceBesideHook([this, id]() { PlaceBeside(id); });
    window->SetPlacementHook([this](const QSize size) { return DefaultPlacement(size); });
    window->SetVisibilityHook(
        [this, id](const bool visible)
        {
            if (QAction* action = Action(id))
                action->setChecked(visible);
            if (!visible)
                ReturnFocusToOwner();
        });
}

PanelWindow* PanelHost::Window(const PanelId id) const
{
    const auto found = windows.find(id);
    return found == windows.end() ? nullptr : found->second.data();
}

QAction* PanelHost::Action(const PanelId id) const
{
    const auto found = actions.find(id);
    return found == actions.end() ? nullptr : found->second.data();
}

void PanelHost::Open(const PanelId id)
{
    PanelWindow* window = Window(id);
    if (window == nullptr)
        return;
    window->Reveal();
    if (QAction* action = Action(id))
        action->setChecked(window->isVisible());
}

void PanelHost::Close(const PanelId id)
{
    if (PanelWindow* window = Window(id))
        window->hide();
    if (QAction* action = Action(id))
        action->setChecked(false);
}

bool PanelHost::IsOpen(const PanelId id) const
{
    const PanelWindow* window = Window(id);
    return window != nullptr && window->isVisible();
}

void PanelHost::Toggle(const PanelId id)
{
    const PanelWindow* window = Window(id);
    if (window != nullptr && window->isVisible() && window->isActiveWindow())
        Close(id);
    else
        Open(id);
    if (QAction* action = Action(id))
        action->setChecked(IsOpen(id));
}

void PanelHost::CloseAll()
{
    for (const PanelSpec& spec : PanelCatalog())
    {
        if (IsOpen(spec.id))
            Close(spec.id);
    }
}

void PanelHost::HideAll(const bool remember)
{
    for (const PanelSpec& spec : PanelCatalog())
    {
        if (!IsOpen(spec.id))
            continue;
        // Kept across repeated calls, so hiding to the tray twice does not forget the first set.
        if (remember && std::find(hiddenForTray.begin(), hiddenForTray.end(), spec.id) == hiddenForTray.end())
            hiddenForTray.push_back(spec.id);
        Close(spec.id);
    }
}

void PanelHost::RestoreHidden()
{
    Restore(std::exchange(hiddenForTray, {}));
}

void PanelHost::Restore(const std::vector<PanelId>& ids)
{
    bool shown = false;
    for (const PanelId id : ids)
    {
        PanelWindow* window = Window(id);
        if (window == nullptr || window->isVisible())
            continue;
        // Shown as it was hidden (a minimised panel comes back minimised), and not activated:
        // the user asked for the main window, not for whichever panel happens to come back last.
        window->show();
        window->raise();
        shown = true;
    }
    // Showing a window activates it on most platforms, so the owner takes the keyboard back.
    if (shown)
        ReturnFocusToOwner();
}

void PanelHost::SetLocked(const bool value)
{
    locked = value;
    for (auto& [id, window] : windows)
    {
        if (window)
            window->SetLocked(locked);
    }
    // The panel shortcuts and menu entries too: a locked window (a companion switch, or shutting
    // down for good) opens no more panels. Open() itself stays available to the window.
    for (auto& [id, action] : actions)
    {
        if (action)
            action->setEnabled(!locked);
    }
}

void PanelHost::SetTitleSuffix(const PanelId id, const QString& suffix)
{
    suffixes[id] = suffix;
    if (PanelWindow* window = Window(id))
        window->SetTitleSuffix(suffix);
    if (QAction* action = Action(id))
        action->setText(QString::fromUtf8(Spec(id).title) + suffix);
}

std::optional<PanelId> PanelHost::IdOf(QWidget* const page) const
{
    if (page == nullptr)
        return std::nullopt;
    for (const PanelSpec& spec : PanelCatalog())
    {
        const PanelWindow* window = Window(spec.id);
        if (window == nullptr)
            continue;
        const QWidget* hosted = window->Page();
        if (page == window || (hosted != nullptr && (hosted == page || hosted->isAncestorOf(page))))
            return spec.id;
    }
    return std::nullopt;
}

QList<QWidget*> PanelHost::Pages() const
{
    QList<QWidget*> pages;
    for (const PanelSpec& spec : PanelCatalog())
    {
        if (const PanelWindow* window = Window(spec.id); window != nullptr && window->Page() != nullptr)
            pages.push_back(window->Page());
    }
    return pages;
}

void PanelHost::SaveOpenSet() const
{
    QStringList keys;
    for (const PanelSpec& spec : PanelCatalog())
    {
        if (IsOpen(spec.id) || std::find(hiddenForTray.begin(), hiddenForTray.end(), spec.id) != hiddenForTray.end())
            keys.push_back(QString::fromLatin1(spec.key));
    }
    QSettings().setValue("panels/openSet", keys);
}

void PanelHost::RestoreOpenSet()
{
    std::vector<PanelId> ids;
    for (const QString& key : QSettings().value("panels/openSet").toStringList())
    {
        if (const auto id = PanelFromKey(key))
            ids.push_back(*id);
    }
    Restore(ids);
}

void PanelHost::SetShowHook(std::function<void(QWidget*)> hook)
{
    showHook = std::move(hook);
    for (auto& [id, window] : windows)
    {
        if (window)
            window->SetShowHook(showHook);
    }
}

void PanelHost::SetResizeHook(const PanelId id, std::function<void(QSize)> hook)
{
    resizeHooks[id] = hook;
    if (PanelWindow* window = Window(id))
        window->SetResizeHook(std::move(hook));
}

void PanelHost::SetSettingsHook(std::function<void(int)> hook)
{
    settingsHook = std::move(hook);
}

void PanelHost::PlaceBeside(const PanelId id)
{
    PanelWindow* window = Window(id);
    QScreen* screen = owner->screen();
    if (window == nullptr || screen == nullptr)
        return;
    if (owner->isMaximized() || owner->isFullScreen())
        owner->showNormal();
    const PanelSpec& spec = Spec(id);
    const QRect ownerFrame = owner->frameGeometry();
    const QSize ownerFrameExtra = ownerFrame.size() - owner->size();
    const BesidePlacement placement =
        PlaceBesideGeometry(ownerFrame, screen->availableGeometry(), spec.defaultSize.width(), spec.minUsefulWidth);
    owner->move(placement.ownerFrame.topLeft()); // a top-level's position is its frame's
    owner->resize(placement.ownerFrame.size() - ownerFrameExtra);
    window->PlaceAt(placement.panelFrame);
    window->Reveal();
}

void PanelHost::SetUnowned(const bool value)
{
    unowned = value;
    for (auto& [id, window] : windows)
    {
        if (window)
            window->SetUnowned(unowned);
    }
}

void PanelHost::SetIconProvider(std::function<QIcon(const char*)> provider)
{
    iconProvider = std::move(provider);
    RefreshIcons();
    for (auto& [id, window] : windows)
    {
        if (window)
            window->SetIconProvider(iconProvider);
    }
}

void PanelHost::RefreshIcons()
{
    for (const PanelSpec& spec : PanelCatalog())
    {
        if (QAction* action = Action(spec.id))
            action->setIcon(iconProvider ? iconProvider(spec.iconName) : QIcon());
    }
}

void PanelHost::SetDefaultFocus(QWidget* const widget)
{
    defaultFocus = widget;
}

void PanelHost::ReturnFocusToOwner()
{
    // Hidden to the tray, or minimised: nothing to hand focus to.
    if (!owner->isVisible() || owner->isMinimized())
        return;
    owner->activateWindow();
    // The last focus widget may have gone since (a chat search hidden by the rail); activation
    // alone would then pick the first widget in the tab chain, not the message box.
    QWidget* target = lastOwnerFocus.data();
    if (target == nullptr || !target->isVisible() || !target->isEnabled())
        target = defaultFocus.data();
    if (target != nullptr && target->isVisible() && target->isEnabled())
        target->setFocus(Qt::OtherFocusReason);
}

QRect PanelHost::DefaultPlacement(const QSize size)
{
    const QRect ownerFrame = owner->frameGeometry();
    const QScreen* screen = owner->screen();
    const QRect available = screen != nullptr ? screen->availableGeometry() : QRect(ownerFrame.topLeft(), size);
    // Each side cascades on its own: the first panel on the right is top-aligned with the owner,
    // the next one opened there steps down.
    const QRect first = PanelPlacement(ownerFrame, available, size, 0);
    int& cascade = first.center().x() >= ownerFrame.center().x() ? cascadeRight : cascadeLeft;
    return PanelPlacement(ownerFrame, available, size, cascade++);
}
}
