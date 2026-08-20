#include "pipelinePanel.h"

#include <QAbstractItemView>
#include <QColor>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace
{
    QColor PhaseColor(const QString& phase)
    {
        if (phase == "Error" || phase == "Unavailable" || phase == "Missing")
        {
            return QColor("#ff8da1");
        }
        if (phase == "Running" || phase == "Thinking" || phase == "Generating" ||
            phase == "Speaking" || phase == "Recording" || phase == "Transcribing" ||
            phase == "Analyzing" || phase == "Capturing" || phase == "Queued" ||
            phase == "Loading" || phase == "Designing" || phase == "Triggered" ||
            phase == "Initiating")
        {
            return QColor("#79d9ff");
        }
        if (phase == "Ready" || phase == "Saved" || phase == "Backfilled" ||
            phase == "Watching")
        {
            return QColor("#69e2c4");
        }
        return QColor("#aab9cd");
    }
}

PipelinePanel::PipelinePanel(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(8);

    summary = new QLabel(
        "Each row is an independently owned lane. Conversation and vision share the "
        "capacity-aware llama scheduler; embeddings, memory, voice, microphone, and "
        "perception keep their own workers.", this);
    summary->setWordWrap(true);
    summary->setObjectName("secondaryText");
    layout->addWidget(summary);

    table = new QTableWidget(0, 5, this);
    table->setHorizontalHeaderLabels({"Pipeline", "State", "Queue", "Last time", "Detail"});
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    layout->addWidget(table, 1);

    for (const QString& component : {
        QStringLiteral("Conversation"), QStringLiteral("Memory"),
        QStringLiteral("Embeddings"), QStringLiteral("Language model"),
        QStringLiteral("Voice"), QStringLiteral("Microphone"),
        QStringLiteral("Vision"), QStringLiteral("Automation"),
        QStringLiteral("Perception"), QStringLiteral("Initiative"),
        QStringLiteral("Input")})
    {
        const int row = EnsureRow(component);
        SetCell(row, 1, "Waiting", QColor("#7f91aa"));
        SetCell(row, 2, "0");
        SetCell(row, 3, "-");
        SetCell(row, 4, "No event yet.");
    }
}

int PipelinePanel::EnsureRow(const QString& component)
{
    const auto found = rows.find(component);
    if (found != rows.end())
    {
        return found->second;
    }
    const int row = table->rowCount();
    table->insertRow(row);
    rows.emplace(component, row);
    SetCell(row, 0, component, QColor("#dceaff"));
    return row;
}

void PipelinePanel::SetCell(
    const int row,
    const int column,
    const QString& text,
    const QColor& color)
{
    QTableWidgetItem* item = table->item(row, column);
    if (item == nullptr)
    {
        item = new QTableWidgetItem();
        table->setItem(row, column, item);
    }
    item->setText(text);
    if (color.isValid())
    {
        item->setForeground(color);
    }
}

void PipelinePanel::Observe(const revia::runtime::RuntimeEvent& event)
{
    if (event.kind != revia::runtime::RuntimeEventKind::ComponentStatus ||
        event.component.empty())
    {
        return;
    }

    const QString component = QString::fromStdString(event.component);
    const QString phase = QString::fromStdString(event.phase);
    const int row = EnsureRow(component);
    SetCell(row, 1, phase, PhaseColor(phase));
    SetCell(row, 2, QString::number(event.queueDepth));
    SetCell(row, 3, event.elapsedMilliseconds >= 0.0
        ? QString::number(event.elapsedMilliseconds, 'f', 1) + " ms"
        : QStringLiteral("-"));
    SetCell(row, 4, QString::fromStdString(event.message));
}
