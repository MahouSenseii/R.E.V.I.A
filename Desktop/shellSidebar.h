#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>

class QAction;
class QBoxLayout;
class QIcon;
class QLabel;
class QMenu;
class QToolButton;
class QVBoxLayout;

namespace revia::desktop
{
// The left sidebar: brand and collapse toggle, a body the window fills with the chat list, and a
// footer with the companion's menu button and Settings. Expanded it is 264px wide; collapsed it is
// a 56px rail of icon buttons (new chat, search, panels) in place of the body.
//
// The window decides the mode. SetExpanded holds the owner's preference (sidebar/expanded);
// SetNarrow(true) forces the rail for a narrow window, where the toggle expands it only until the
// window widens again. Clicking the toggle reports toggled(expanded, persisted): persisted is false
// for those temporary narrow-window changes. SetExpanded and SetNarrow never call toggled.
class ShellSidebar final : public QWidget
{
  public:
    static constexpr int ExpandedWidth = 264;
    static constexpr int RailWidth = 56;

    struct Callbacks
    {
        std::function<void()> newChat, search, panels, settings;
        std::function<void(bool expanded, bool persisted)> toggled;
    };

    ShellSidebar(Callbacks callbacks, QWidget* parent = nullptr);

    // Shown only while expanded; the sidebar does not take ownership away from the caller.
    void SetBody(QWidget* body);
    // Opened by the footer menu button above itself, and by the rail's panels button when no panels
    // callback is set.
    void SetMenu(QMenu* menu);
    // The rail's panels button opens the menu beside itself with this action under the pointer
    // (the first panel entry), so the menu opens at its Panels section.
    void SetPanelsAnchor(QAction* action);
    // The companion name (its first letter is the avatar) and a detail line (the active profile).
    void SetAccount(const QString& name, const QString& detail);
    // The dot on the avatar: Activity holds warnings or errors.
    void SetAttention(bool attention);
    void SetExpanded(bool expanded);
    void SetNarrow(bool narrow);
    [[nodiscard]] bool IsExpanded() const;
    [[nodiscard]] bool IsNarrow() const
    {
        return narrow;
    }
    [[nodiscard]] QToolButton* ToggleButton() const;
    // Icons by name (panel-left, plus, search, grid, settings); refreshed when the palette or
    // style changes.
    void SetIconProvider(std::function<QIcon(const char*)> provider);
    // Opens the menu above the footer button (what clicking it does).
    void PopupMenu();

  protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void ApplyMode();
    void ApplyIcons();
    void ApplyAccountText();
    void RefreshBrandMark();

    Callbacks callbacks;
    QWidget* header = nullptr;
    QLabel* brandMark = nullptr;
    QLabel* brand = nullptr;
    QToolButton* toggle = nullptr;
    QWidget* bodyHost = nullptr;
    QVBoxLayout* bodyLayout = nullptr;
    QPointer<QWidget> body;
    QToolButton* railNewChat = nullptr;
    QToolButton* railSearch = nullptr;
    QToolButton* railPanels = nullptr;
    QWidget* railStretch = nullptr;
    QWidget* footer = nullptr;
    QBoxLayout* footerLayout = nullptr;
    QToolButton* menuButton = nullptr;
    QBoxLayout* menuButtonLayout = nullptr;
    QLabel* avatar = nullptr;
    QLabel* accountName = nullptr;
    QLabel* accountDetail = nullptr;
    QLabel* attentionDot = nullptr;
    QToolButton* settingsButton = nullptr;
    QPointer<QMenu> menu;
    QPointer<QAction> panelsAnchor;
    std::function<QIcon(const char*)> iconProvider;
    QString name;
    QString detail;
    bool preferredExpanded = true;
    bool narrow = false;
    bool temporarilyExpanded = false;
};
}
