#pragma once

#include <QKeySequence>
#include <QList>
#include <QMetaObject>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

#include <functional>
#include <map>
#include <optional>
#include <vector>

class QAction;
class QCloseEvent;
class QHideEvent;
class QIcon;
class QKeyEvent;
class QResizeEvent;
class QShowEvent;
class QToolButton;
class QVBoxLayout;

namespace revia::desktop
{
// The secondary pages that open as their own windows beside the chat.
enum class PanelId
{
    Canvas,
    Vision,
    Profiles,
    Mind,
    Memory,
    Voice,
    Presence,
    Audience,
    Agents,
    Learning,
    Development,
    Activity,
    Pipelines,
    InternetActivity,
    Resources
};

struct PanelSpec
{
    PanelId id;
    const char* key;   // QSettings and objectName key: panelWindow_<key>, panels/<key>/geometry
    const char* title; // window title before " — Revia", menu and palette text
    const char* group; // the sidebar menu section: Workspace, Companion, Studio or Runtime
    const char* iconName;
    const char* pageObjectName; // the existing page the window hosts
    QKeySequence shortcut;
    QSize defaultSize;
    int minUsefulWidth;
    // A SettingsSection value (settingsTypes.h), or -1 when the panel has no Settings section.
    // Kept as an int so the shell widgets do not depend on the settings headers.
    int settingsSection;
    // An in-page heading that only repeats the window title; hidden while the page is hosted.
    const char* redundantTitleObjectName;
};

[[nodiscard]] const std::vector<PanelSpec>& PanelCatalog();
[[nodiscard]] const PanelSpec& Spec(PanelId id);
// Accepts a key ("memory") or a title ("Memory", "Internet activity"), ignoring case.
[[nodiscard]] std::optional<PanelId> PanelFromKey(const QString& keyOrTitle);

// Where a panel opens when it has no saved geometry, as a frame rectangle: to the right of the
// owner with a 12px gap if it fits on the available area, else to its left, else flush with the
// right edge of the available area. cascadeIndex steps the panel down 32px per window already
// placed on that side this session. The size is clamped to the available area.
[[nodiscard]] QRect PanelPlacement(const QRect& ownerFrame, const QRect& available, QSize size, int cascadeIndex);

// "Place beside chat": the owner keeps its height and moves to the left edge, narrowed so a panel
// of max(minUsefulWidth, min(defaultWidth, 40% of the available width)) fits to its right with a
// 12px gap. The owner never gets narrower than 760; the panel then overlaps it at the right edge.
struct BesidePlacement
{
    QRect ownerFrame;
    QRect panelFrame;
};
[[nodiscard]] BesidePlacement PlaceBesideGeometry(const QRect& ownerFrame, const QRect& available, int defaultWidth, int minUsefulWidth);

// A top-level window around one existing page. It is a Qt child of the main window, so the page
// stays reachable through window.findChild, and it is only ever hidden, never deleted, so widgets
// that runtime events update always exist.
class PanelWindow final : public QWidget
{
  public:
    PanelWindow(const PanelSpec& spec, QWidget* owner);

    // Takes the page (reparented and shown) below the header. A previous page is hidden and left
    // to its owner.
    void SetPage(QWidget* page);
    [[nodiscard]] QWidget* Page() const;
    // Re-applies what SetPage does to the page's chrome: the 20/16 margin floor and the hidden
    // redundant heading. Runs on every show; call it after the page's contents were rebuilt.
    void PolishPage();
    // " (3)" makes "Activity (3) — Revia".
    void SetTitleSuffix(const QString& suffix);
    // Shows (restoring from minimised), raises and activates.
    void Reveal();
    // Moves to a frame rectangle and counts as placed, so the first show neither restores the
    // saved geometry nor runs the default placement. While the window is hidden the rectangle
    // waits for the next show, where the frame margins it needs are known.
    void PlaceAt(const QRect& frame);
    void SetLocked(bool locked);
    // Runs once, on the first show (the native frame colours).
    void SetShowHook(std::function<void(QWidget*)> hook);
    // Runs on every resize and on every show.
    void SetResizeHook(std::function<void(QSize)> hook);
    void SetSettingsHook(std::function<void()> hook);
    void SetPlaceBesideHook(std::function<void()> hook);
    // Gives the frame rectangle for a first show without saved geometry (PanelHost's cascade),
    // from the window's frame size.
    void SetPlacementHook(std::function<QRect(QSize)> hook);
    // Runs after every non-spontaneous show (true) and hide (false).
    void SetVisibilityHook(std::function<void(bool visible)> hook);
    // Header icons by name ("columns", "settings"); refreshed when the palette or style changes.
    void SetIconProvider(std::function<QIcon(const char*)> provider);
    // Drops the transient parent before every show, so the main window can be raised above the
    // panel (an experiment for Windows; see PanelHost::SetUnowned).
    void SetUnowned(bool unowned);
    [[nodiscard]] const PanelSpec& Specification() const
    {
        return spec;
    }

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    [[nodiscard]] bool RestoreGeometry();
    void SaveGeometry() const;
    // The frame's size beyond the client area. Accurate only while the native window exists and
    // the window counts as visible (from showEvent on); Qt reports nothing usable before that.
    [[nodiscard]] QSize FrameExtra() const;
    // move() places a top-level window's frame but resize() sizes its client area.
    void ApplyFrame(const QRect& frame);
    void ApplyIcons();
    void ApplyUnowned();

    PanelSpec spec;
    QString suffix;
    QVBoxLayout* body = nullptr;
    QWidget* header = nullptr;
    QToolButton* placeBeside = nullptr;
    QToolButton* openSettings = nullptr;
    QPointer<QWidget> page;
    std::function<void(QWidget*)> showHook;
    std::function<void(QSize)> resizeHook;
    std::function<void()> settingsHook;
    std::function<void()> placeBesideHook;
    std::function<QRect(QSize)> placementHook;
    std::function<void(bool)> visibilityHook;
    std::function<QIcon(const char*)> iconProvider;
    std::optional<QRect> pendingFrame; // PlaceAt while hidden
    bool placed = false;
    bool shownOnce = false;
    bool unowned = false;
};

// Owns the panel windows' lifecycle for the main window: one window per panel, its checkable menu
// action, placement, locking, hide-to-tray and the open set restored at startup. Not a QObject;
// the windows and actions are Qt children of the owner.
class PanelHost final
{
  public:
    explicit PanelHost(QWidget* owner);
    ~PanelHost();
    PanelHost(const PanelHost&) = delete;
    PanelHost& operator=(const PanelHost&) = delete;

    // Creates the panel's window on first use and hosts the page in it. Calling it again for the
    // same panel keeps the window and swaps the page.
    void Adopt(PanelId id, QWidget* page);
    [[nodiscard]] PanelWindow* Window(PanelId id) const;
    // Checkable, checked while the window is visible; carries the panel's application shortcut.
    // Triggering it opens the panel, or closes it when it is already the active window.
    [[nodiscard]] QAction* Action(PanelId id) const;
    void Open(PanelId id);
    void Close(PanelId id);
    [[nodiscard]] bool IsOpen(PanelId id) const;
    void CloseAll();
    // remember=true records the open windows for RestoreHidden (hide to tray).
    void HideAll(bool remember);
    // Shows the windows HideAll(true) hid without activating them: the owner keeps (or gets back)
    // the active window and its last focus widget. Show the owner first.
    void RestoreHidden();
    // Disables the panels' bodies and their actions (shortcuts and menu entries). The windows
    // still close, and Open() still works for the owner's own use.
    void SetLocked(bool locked);
    // Window title and action text: SetTitleSuffix(Activity, " (3)") gives "Activity (3)".
    void SetTitleSuffix(PanelId id, const QString& suffix);
    // The panel hosting page, or one of its descendants (Mind's views map to Mind).
    [[nodiscard]] std::optional<PanelId> IdOf(QWidget* page) const;
    // The hosted pages in catalog order.
    [[nodiscard]] QList<QWidget*> Pages() const;
    // QSettings panels/openSet: the open panels (or those hidden to the tray). Call it before
    // HideAll(false) at shutdown; RestoreOpenSet reopens them the way RestoreHidden does.
    void SaveOpenSet() const;
    void RestoreOpenSet();
    void SetShowHook(std::function<void(QWidget*)> hook);
    void SetResizeHook(PanelId id, std::function<void(QSize)> hook);
    // Receives the panel's settingsSection when its gear is clicked.
    void SetSettingsHook(std::function<void(int section)> hook);
    void PlaceBeside(PanelId id);
    void SetUnowned(bool unowned);
    // Icons for the actions and the window headers, by PanelSpec::iconName. RefreshIcons re-asks
    // the provider (after a theme change); the windows refresh their own headers.
    void SetIconProvider(std::function<QIcon(const char*)> provider);
    void RefreshIcons();
    // Focus returns here when a panel closes and the owner's last focus widget is missing,
    // hidden or disabled.
    void SetDefaultFocus(QWidget* widget);

  private:
    void Toggle(PanelId id);
    void Restore(const std::vector<PanelId>& ids);
    void Configure(PanelWindow* window);
    void ReturnFocusToOwner();
    [[nodiscard]] QRect DefaultPlacement(QSize size);

    QWidget* owner;
    std::map<PanelId, QPointer<PanelWindow>> windows;
    std::map<PanelId, QPointer<QAction>> actions;
    std::map<PanelId, QString> suffixes;
    std::map<PanelId, std::function<void(QSize)>> resizeHooks;
    std::vector<PanelId> hiddenForTray;
    std::vector<QMetaObject::Connection> connections;
    std::function<void(QWidget*)> showHook;
    std::function<void(int)> settingsHook;
    std::function<QIcon(const char*)> iconProvider;
    QPointer<QWidget> lastOwnerFocus;
    QPointer<QWidget> defaultFocus;
    int cascadeRight = 0;
    int cascadeLeft = 0;
    bool locked = false;
    bool unowned = false;
};
}
