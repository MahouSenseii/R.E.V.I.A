#pragma once

#include "Runtime/runtimeEvents.h"

#include <QColor>
#include <QWidget>

#include <map>

class QLabel;
class QTableWidget;

// Presentation-only view of independent runtime lanes. It consumes events and never
// queries, starts, stops, or otherwise owns a backend service.
class PipelinePanel final : public QWidget
{
public:
    explicit PipelinePanel(QWidget* parent = nullptr);

    void Observe(const revia::runtime::RuntimeEvent& event);

private:
    int EnsureRow(const QString& component);
    void SetCell(int row, int column, const QString& text, const QColor& color = {});

    QTableWidget* table = nullptr;
    QLabel* summary = nullptr;
    std::map<QString, int> rows;
};
