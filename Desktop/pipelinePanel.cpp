#include "pipelinePanel.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace
{
    using revia::desktop::theme::Role;

    // Holds the Role of a toned cell; the colour itself is derived from the current theme.
    constexpr int ToneRole = Qt::UserRole + 7;

    Role PhaseTone(const QString& phase)
    {
        if (phase == "Error" || phase == "Unavailable" || phase == "Missing")
        {
            return Role::Danger;
        }
        if (phase == "Degraded" || phase == "Hard only" || phase == "Flagged")
        {
            return Role::Warning;
        }
        if (phase == "Running" || phase == "Thinking" || phase == "Generating" ||
            phase == "Speaking" || phase == "Recording" || phase == "Transcribing" ||
            phase == "Analyzing" || phase == "Capturing" || phase == "Queued" ||
            phase == "Loading" || phase == "Designing" || phase == "Triggered" ||
            phase == "Initiating" || phase == "Considering" ||
            phase == "Researching" || phase == "SpeechDetected" ||
            phase == "Received")
        {
            return Role::Accent;
        }
        if (phase == "Ready" || phase == "Generated" || phase == "Saved" || phase == "Backfilled" ||
            phase == "Watching" || phase == "Healthy" || phase == "HandsFree" ||
            phase == "Replied" || phase == "Spoke")
        {
            return Role::Success;
        }
        return Role::TextSecondary;
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

    table = new QTableWidget(0, 6, this);
    table->setHorizontalHeaderLabels(
        {"Pipeline", "Compute", "State", "Queue", "Last time", "Detail"});
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
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
        QStringLiteral("Internet"), QStringLiteral("Permissions"),
        QStringLiteral("Response filters"),
        QStringLiteral("Conversation quality"),
        QStringLiteral("Perception"), QStringLiteral("Initiative"),
        QStringLiteral("Curiosity"), QStringLiteral("Browser"),
        QStringLiteral("Input"), QStringLiteral("Presence"),
        QStringLiteral("Avatar"), QStringLiteral("Adapters")})
    {
        const int row = EnsureRow(component);
        SetCell(row, 1, "-");
        SetCell(row, 2, "Waiting", Role::TextSecondary);
        SetCell(row, 3, "0");
        SetCell(row, 4, "-");
        SetCell(row, 5, "No event yet.");
    }
    themeListener = revia::desktop::theme::AddListener([this]() { ApplyTones(); });
}

PipelinePanel::~PipelinePanel()
{
    revia::desktop::theme::RemoveListener(themeListener);
}

void PipelinePanel::ApplyTones()
{
    for (int row = 0; row < table->rowCount(); ++row)
    {
        for (int column = 0; column < table->columnCount(); ++column)
        {
            QTableWidgetItem* item = table->item(row, column);
            if (item != nullptr && item->data(ToneRole).isValid())
            {
                item->setForeground(revia::desktop::theme::RoleColor(static_cast<Role>(item->data(ToneRole).toInt())));
            }
        }
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
    SetCell(row, 0, component);
    return row;
}

void PipelinePanel::SetCell(const int row, const int column, const QString& text, const std::optional<Role> tone)
{
    QTableWidgetItem* item = table->item(row, column);
    if (item == nullptr)
    {
        item = new QTableWidgetItem();
        table->setItem(row, column, item);
    }
    item->setText(text);
    if (tone.has_value())
    {
        item->setData(ToneRole, static_cast<int>(*tone));
        item->setForeground(revia::desktop::theme::RoleColor(*tone));
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
    if (!event.resource.empty())
    {
        SetCell(row, 1, QString::fromStdString(event.resource));
    }
    SetCell(row, 2, phase, PhaseTone(phase));
    SetCell(row, 3, QString::number(event.queueDepth));
    SetCell(row, 4, event.elapsedMilliseconds >= 0.0
        ? QString::number(event.elapsedMilliseconds, 'f', 1) + " ms"
        : QStringLiteral("-"));
    SetCell(row, 5, QString::fromStdString(event.message));
}
