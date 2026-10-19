#pragma once

#include "Runtime/runtimeEvents.h"
#include "theme.h"

#include <QWidget>

#include <map>
#include <optional>

class QLabel;
class QTableWidget;

// Presentation-only view of independent runtime lanes. It consumes events and never
// queries, starts, stops, or otherwise owns a backend service.
class PipelinePanel final : public QWidget
{
public:
    explicit PipelinePanel(QWidget* parent = nullptr);
    ~PipelinePanel() override;

    void Observe(const revia::runtime::RuntimeEvent& event);

private:
    int EnsureRow(const QString& component);
    // A tone is stored on the item and turned into a colour now and after every theme change.
    void SetCell(int row, int column, const QString& text, std::optional<revia::desktop::theme::Role> tone = {});
    void ApplyTones();

    QTableWidget* table = nullptr;
    QLabel* summary = nullptr;
    std::map<QString, int> rows;
    int themeListener = 0;
};
