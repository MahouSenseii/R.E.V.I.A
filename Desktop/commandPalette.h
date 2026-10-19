#pragma once

#include <QIcon>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <vector>

class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QMouseEvent;
class QPaintEvent;
class QTimer;
class QAction;

namespace revia::desktop
{
// One thing the palette can run. group places it under a header: Actions, Panels, Settings, Chats
// or Companions (other names follow them, in order of first appearance). id must be stable; it is
// what palette/recent remembers.
struct CommandEntry
{
    QString id, title, subtitle, group, shortcutText;
    QIcon icon;
    QStringList keywords;
    std::function<void()> run;
    // Unset means always enabled. Disabled entries are drawn greyed and cannot be chosen.
    std::function<bool()> enabled;
    // Added to the match score while a query is typed, and orders entries that score the same.
    int priority = 0;
};

// How well query matches candidate, ignoring case: 1000 when the candidate starts with it, 600
// when a word in the candidate does, 300 when it appears anywhere, 100 minus the gaps when its
// characters appear in order, -1 when it does not match. An empty query scores 0.
[[nodiscard]] int FuzzyScore(const QString& query, const QString& candidate);

// Ctrl+K: a search box over everything the window can do, as an overlay that covers its host (the
// root panel) with a scrim and a card. The window builds the entries; the palette filters, groups
// and runs them. With an empty query the last six entries run (QSettings palette/recent) come
// first; with a query, groups are ordered by their best match so the top row is the best one.
class CommandPalette final : public QWidget
{
  public:
    explicit CommandPalette(QWidget* host);

    void SetCommands(std::vector<CommandEntry> commands);
    // Asked again 150 ms after the query stops changing (and at once on opening); its entries are
    // already filtered by the source and at most eight are shown.
    void SetDynamicSource(std::function<std::vector<CommandEntry>(const QString&)> source);
    // When it returns false (navigation is locked) the palette does not open, and an open one
    // closes instead of running an entry. The window should still Close() it when it locks.
    void SetAvailable(std::function<bool()> available);
    void Open(const QString& query = {});
    // Only chats: the rail's search button and Ctrl+F while the sidebar is a rail.
    void OpenChats();
    void Close();
    // Open from Open() until Close(), Esc, a click outside the card, a run, or the host hiding (to
    // the tray). A minimised window keeps it open. While open, its input keeps the window's focus.
    [[nodiscard]] bool IsOpen() const;
    // The search icon in the input, by name.
    void SetIconProvider(std::function<QIcon(const char*)> provider);
    // The highlighted entry's id, empty when nothing is highlighted.
    [[nodiscard]] QString CurrentId() const;
    // Runs the highlighted entry (what Enter and a click do). False when there is none, it is
    // disabled, or the palette is no longer available (it then closes).
    bool RunCurrent();
    // Runs the pending dynamic query now instead of after the debounce.
    void FlushDynamicSource();
    [[nodiscard]] bool IsChatsMode() const
    {
        return chatsOnly;
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void OpenWith(const QString& query, bool chats);
    void FetchDynamic();
    void Rebuild(bool keepSelection);
    void Move(int step);
    void LayoutCard();
    void Remember(const QString& id) const;
    [[nodiscard]] bool IsRunnable(int row) const;

    QPointer<QWidget> host;
    QFrame* card = nullptr;
    QLineEdit* input = nullptr;
    QAction* searchIcon = nullptr;
    QListWidget* list = nullptr;
    QLabel* empty = nullptr;
    QLabel* footer = nullptr;
    QTimer* dynamicDelay = nullptr;
    std::vector<CommandEntry> commands;
    std::vector<CommandEntry> dynamicEntries;
    QString dynamicQuery; // the query dynamicEntries answer
    // What the list shows, in row order; items refer to it by index.
    std::vector<CommandEntry> visible;
    std::function<std::vector<CommandEntry>(const QString&)> dynamicSource;
    std::function<bool()> available;
    std::function<QIcon(const char*)> iconProvider;
    QPointer<QWidget> previousFocus;
    bool chatsOnly = false;
};
}
