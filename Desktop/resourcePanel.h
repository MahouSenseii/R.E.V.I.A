#pragma once

#include "Runtime/runtimeEvents.h"

#include <QWidget>

#include <map>

class QTableWidget;

// Presentation-only view of the startup inventory and immutable resource plan. The
// runtime owns detection and placement; this panel cannot start, move, or stop a worker.
class ResourcePanel final : public QWidget
{
public:
    explicit ResourcePanel(QWidget* parent = nullptr);

    void Observe(const revia::runtime::RuntimeEvent& event);

private:
    int EnsureHardwareRow(const QString& name);
    int EnsureAssignmentRow(const QString& workload);
    static void SetCell(QTableWidget* table, int row, int column, const QString& text);

    QTableWidget* hardwareTable = nullptr;
    QTableWidget* assignmentTable = nullptr;
    std::map<QString, int> hardwareRows;
    std::map<QString, int> assignmentRows;
};
