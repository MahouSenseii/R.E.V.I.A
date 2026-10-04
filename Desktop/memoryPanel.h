#pragma once

#include "Memory/memoryTypes.h"
#include "Runtime/reviaSession.h"
#include "Memory/memoryRevision.h"

#include <QPointer>
#include <QWidget>

#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QDialog;

// Stored records remain read-only; exact owner revisions go through the session boundary.
class MemoryPanel final : public QWidget
{
  public:
    explicit MemoryPanel(revia::runtime::ReviaSession& session, QWidget* parent = nullptr);

    void Refresh();
    void SetRevisionSubmitter(std::function<bool(const revia::memory::MemoryRevisionRequest&)> submitter);
    void SetRevisionBusy(bool busy);
    void SetRevisionOutcome(bool success, const std::string& message);

  private:
    void ApplyFilter();
    void Render(const std::vector<memoryEntry>& entries);
    void UpdateSelectedRevision();
    void ReviseSelectedMemory();
    void SelectLinkedMemory(const QString& link);

    revia::runtime::ReviaSession& session;

    QLabel* statusLabel = nullptr;
    QLineEdit* searchInput = nullptr;
    QPushButton* refreshButton = nullptr;
    QCheckBox* highImportanceOnly = nullptr;
    QTableWidget* table = nullptr;
    QLabel* revisionProvenance = nullptr;
    QLabel* revisionOutcome = nullptr;
    QPushButton* reviseButton = nullptr;
    QPointer<QDialog> revisionDialog;
    std::vector<memoryEntry> displayedEntries;
    revia::runtime::RuntimeStamp memorySnapshotOrigin;
    std::uint64_t memorySnapshotAudienceRevision = 0;
    std::function<bool(const revia::memory::MemoryRevisionRequest&)> submitRevision;
    bool revisionBusy = false;
};
