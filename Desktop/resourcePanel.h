#pragma once

#include "Runtime/runtimeEvents.h"

#include <QWidget>

#include <map>

class QLabel;
class QProgressBar;
class QTableWidget;

// Presentation-only view of the startup inventory, the immutable resource plan, and the
// live readings measured against it. The runtime owns detection, placement, and
// sampling; this panel cannot start, move, or stop a worker, and a reading it draws
// never feeds back into the plan.
class ResourcePanel final : public QWidget
{
public:
    explicit ResourcePanel(QWidget* parent = nullptr);

    void Observe(const revia::runtime::RuntimeEvent& event);

private:
    struct UsageRow
    {
        int row = 0;
        QProgressBar* bar = nullptr;
    };

    int EnsureHardwareRow(const QString& name);
    int EnsureAssignmentRow(const QString& workload);
    UsageRow& EnsureUsageRow(const QString& label);
    void ApplyUsage(const revia::runtime::RuntimeEvent& event);
    static void SetCell(QTableWidget* table, int row, int column, const QString& text);
    // Grows a table to exactly the height its rows need. Three tables sharing one
    // stretchy column is what made the section headings overlap them: each asked for the
    // same space, and none of them fit. The page scrolls instead.
    static void FitToContents(QTableWidget* table);

    QTableWidget* usageTable = nullptr;
    QTableWidget* hardwareTable = nullptr;
    QTableWidget* assignmentTable = nullptr;
    std::map<QString, UsageRow> usageRows;
    std::map<QString, int> hardwareRows;
    std::map<QString, int> assignmentRows;
};
