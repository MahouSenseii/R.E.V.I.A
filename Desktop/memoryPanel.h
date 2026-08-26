#pragma once

#include "Runtime/reviaSession.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

// Shows what Revia actually remembers.
//
// Read-only, and deliberately so. Memory is written through the reviewed memory path --
// the memory agent proposes, the significance filter decides, an approved lesson becomes
// an ordinary entry. A viewer that could also write would be a second, unreviewed way
// in, which is exactly the thing that path exists to prevent.
//
// The value of showing it at all is that "what does she know about me" stops being
// something you infer from what she happens to bring up.
class MemoryPanel final : public QWidget
{
public:
    explicit MemoryPanel(
        revia::runtime::ReviaSession& session,
        QWidget* parent = nullptr);

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
