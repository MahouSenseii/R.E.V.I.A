#pragma once

#include "Attachments/documentStore.h"
#include "Core/conversationMessage.h"

#include <QDialog>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

// What the Documents list may do. The window supplies these from ReviaSession.
struct DocumentControls
{
    std::function<std::vector<revia::attachments::StoredDocument>()> list;
    std::function<std::string(const std::string& id)> preview;
    std::function<std::optional<conversationImage>(const std::string& id)> imagePreview;
    std::function<bool(const std::string& id, std::string& error)> forget;
    std::function<void(const std::string& id)> videoPreview;
};

// The files Revia has kept from earlier messages: view one, or forget it -- which
// deletes the stored copy, its passages and their search vectors.
class DocumentsDialog final : public QDialog
{
  public:
    explicit DocumentsDialog(DocumentControls controls, QWidget* parent = nullptr);
    void Refresh();
    [[nodiscard]] std::string SelectedId() const;
    void Select(const std::string& id);
    void ViewSelected();
    // Without the confirmation question, for the button handler after it asked.
    bool ForgetDocument(const std::string& id, QString& error);
    [[nodiscard]] int RowCount() const;
    [[nodiscard]] QString PreviewText() const;

  private:
    void UpdateButtons();
    void ForgetSelected();

    DocumentControls controls;
    std::vector<revia::attachments::StoredDocument> documents;
    QTableWidget* table = nullptr;
    QPlainTextEdit* preview = nullptr;
    QLabel* image = nullptr;
    QLabel* status = nullptr;
    QPushButton* view = nullptr;
    QPushButton* forget = nullptr;
};
