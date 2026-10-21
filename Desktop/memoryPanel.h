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
class QComboBox;

// Stored records remain read-only; exact owner revisions go through the session boundary.
class MemoryPanel final : public QWidget
{
  public:
    explicit MemoryPanel(revia::runtime::ReviaSession& session, QWidget* parent = nullptr);

    void Refresh();
    void SetRevisionSubmitter(std::function<bool(const revia::memory::MemoryRevisionRequest&)> submitter);
    void SetCorrectionProposalSubmitter(std::function<bool(const std::string&, bool)> submitter);
    void SetRevisionBusy(bool busy);
    void SetRevisionOutcome(bool success, const std::string& message);

  private:
    void ApplyFilter();
    void Render(const std::vector<memoryEntry>& entries);
    void UpdateSelectedRevision();
    void ReviseSelectedMemory();
    void RefreshCorrectionProposals();
    void ReviewSelectedCorrection();
    void SubmitCorrectionProposal(const std::string& id, bool accept);
    void ShowRevisionDialog(const memoryEntry& original, revia::memory::MemoryRevisionRequest request,
        const std::string& proposalId = {});
    void SelectLinkedMemory(const QString& link);

    revia::runtime::ReviaSession& session;

    QLabel* statusLabel = nullptr;
    QLabel* emptyLabel = nullptr;
    QLineEdit* searchInput = nullptr;
    QPushButton* refreshButton = nullptr;
    QCheckBox* highImportanceOnly = nullptr;
    QTableWidget* table = nullptr;
    QLabel* revisionProvenance = nullptr;
    QLabel* revisionOutcome = nullptr;
    QPushButton* reviseButton = nullptr;
    QComboBox* correctionSelector = nullptr;
    QPushButton* reviewCorrectionButton = nullptr;
    QPushButton* rejectCorrectionButton = nullptr;
    QLabel* correctionStatus = nullptr;
    std::vector<revia::memory::MemoryCorrectionProposal> correctionProposals;
    std::string activeCorrectionProposalId;
    QPointer<QDialog> revisionDialog;
    std::vector<memoryEntry> displayedEntries;
    revia::runtime::RuntimeStamp memorySnapshotOrigin;
    std::uint64_t memorySnapshotAudienceRevision = 0;
    std::function<bool(const revia::memory::MemoryRevisionRequest&)> submitRevision;
    std::function<bool(const std::string&, bool)> submitCorrectionProposal;
    bool revisionBusy = false;
};
