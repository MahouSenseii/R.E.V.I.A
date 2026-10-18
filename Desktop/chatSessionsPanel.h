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

// Past conversations from the archive: new chat, search, rename, delete and export.
class ChatSessionsPanel final : public QWidget
{
  public:
    explicit ChatSessionsPanel(ChatSessionControls controls, QWidget* parent = nullptr);
    void Refresh();
    // The file half of Export, separate from the dialog so it can be driven directly.
    bool ExportSelected(const QString& path, bool json, QString& error);
    [[nodiscard]] std::string SelectedId() const;
    void SetStatus(const QString& text);

  private:
    void UpdateButtons();
    void RenameSelected();
    void DeleteSelected();
    void ChooseExport(bool json);

    ChatSessionControls controls;
    QLineEdit* search = nullptr;
    QListWidget* list = nullptr;
    QPushButton* newChat = nullptr;
    QPushButton* rename = nullptr;
    QPushButton* remove = nullptr;
    QPushButton* exportMarkdown = nullptr;
    QPushButton* exportJson = nullptr;
    QLabel* status = nullptr;
    QTimer* searchDelay = nullptr;
};
