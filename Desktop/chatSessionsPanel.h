#pragma once

#include "Memory/conversationArchive.h"

#include <QWidget>

#include <functional>
#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;

// What the chat list may do. The window supplies these from ReviaSession, which applies
// the audience and busy checks; the panel only presents the outcome.
struct ChatSessionControls
{
    std::function<std::vector<revia::memory::ArchivedSession>(const std::string& query, std::string& error)> list;
    std::function<std::string()> current;
    std::function<bool(std::string& error)> startNew;
    std::function<bool(const std::string& id, const std::string& title, std::string& error)> rename;
    std::function<bool(const std::string& id, std::string& error)> remove;
    std::function<bool(const std::string& id, bool json, std::string& text, std::string& error)> exportText;
    // Shows the chat. Opening the current one returns to the live transcript.
    std::function<void(const std::string& id)> open;
};

// Past conversations from the archive, as the sidebar's body: New chat, search, a "Recents"
// heading that folds the list, and one line per chat. Rename, export and delete live in each
// row's context menu (and in the chat header's menu for the current chat); F2 and Delete act on
// the highlighted row while the list has focus, or on the current chat when no row is.
class ChatSessionsPanel final : public QWidget
{
  public:
    explicit ChatSessionsPanel(ChatSessionControls controls, QWidget* parent = nullptr);
    void Refresh();
    // The file half of Export, separate from the dialog so it can be driven directly.
    bool ExportSelected(const QString& path, bool json, QString& error);
    [[nodiscard]] std::string SelectedId() const;
    void SetStatus(const QString& text);
    // What New chat does: the window's start, then a fresh list.
    void StartNewChat();
    // Folds the list under its "Recents" heading without touching the rest of the panel.
    void SetListVisible(bool visible);
    [[nodiscard]] QPushButton* ListToggle() const
    {
        return listToggle;
    }
    // The title a chat is listed under (its own title, else its opening line).
    [[nodiscard]] QString TitleFor(const std::string& id) const;
    // The current chat's title, from the last refresh; "New chat" before it has one.
    [[nodiscard]] QString CurrentTitle() const;
    // The row actions, for any chat; they ask, then report in the status line.
    void RenameChat(const std::string& id);
    void DeleteChat(const std::string& id);
    void ExportChat(const std::string& id, bool json);
    // The panel takes whatever height the sidebar gives it. Its wrapped status line would
    // otherwise report a height for its width and push the sidebar footer off the window.
    [[nodiscard]] bool hasHeightForWidth() const override
    {
        return false;
    }

  private:
    void ShowRowMenu(const QPoint& position);
    [[nodiscard]] std::string CurrentId() const;
    // What F2 and Delete act on: the highlighted row, else the current chat.
    [[nodiscard]] std::string TargetId() const;
    bool WriteExport(const std::string& id, const QString& path, bool json, QString& error);

    ChatSessionControls controls;
    QLineEdit* search = nullptr;
    QListWidget* list = nullptr;
    QPushButton* newChat = nullptr;
    QPushButton* listToggle = nullptr;
    QPushButton* rename = nullptr;
    QPushButton* remove = nullptr;
    QPushButton* exportChat = nullptr;
    QLabel* status = nullptr;
    QTimer* searchDelay = nullptr;
    // From the last refresh, so the title stays right while the panel is folded or hidden.
    std::vector<revia::memory::ArchivedSession> sessions;
    bool loadFailed = false;
};
