#pragma once

#include "Memory/memoryTypes.h"
#include "Runtime/reviaSession.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

// Read-only view of stored memories; writes remain behind the reviewed memory path.
class MemoryPanel final : public QWidget
{
public:
    explicit MemoryPanel(revia::runtime::ReviaSession& session, QWidget* parent = nullptr);

    void Refresh();

private:
    void ApplyFilter();
    void Render(const std::vector<memoryEntry>& entries);

    revia::runtime::ReviaSession& session;

    QLabel* statusLabel = nullptr;
    QLineEdit* searchInput = nullptr;
    QPushButton* refreshButton = nullptr;
    QCheckBox* highImportanceOnly = nullptr;
    QTableWidget* table = nullptr;
};
