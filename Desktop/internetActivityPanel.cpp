#include "internetActivityPanel.h"

#include "theme.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

#include <chrono>

namespace
{
constexpr int MaximumVisibleLookups = 200;
constexpr int IdentityRole = Qt::UserRole;
constexpr int DetailRole = Qt::UserRole + 1;
constexpr int ToneRole = Qt::UserRole + 7;

std::string ActivityIdentity(const revia::runtime::RuntimeEvent& event)
{
    const std::string initiator = event.initiator.empty() ? "Unknown" : event.initiator;
    return initiator + ":" + std::to_string(event.turnId);
}

QString EventTime(const revia::runtime::RuntimeEvent& event)
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        event.occurredAt.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(milliseconds).toString("HH:mm:ss.zzz");
}

revia::desktop::theme::Role PhaseTone(const QString& phase)
{
    using revia::desktop::theme::Role;
    if (phase == QStringLiteral("Ready")) return Role::Success;
    if (phase == QStringLiteral("Searching")) return Role::Accent;
    if (phase == QStringLiteral("Unavailable")) return Role::Danger;
    return Role::TextSecondary;
}

void SetCell(QTableWidget* table, const int row, const int column, const QString& text)
{
    QTableWidgetItem* item = table->item(row, column);
    if (item == nullptr)
    {
        item = new QTableWidgetItem();
        table->setItem(row, column, item);
    }
    item->setText(text);
}
}

InternetActivityPanel::InternetActivityPanel(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(8);

    auto* summary = new QLabel(
        "Every lookup appears here with its initiator, exact query, browser/provider, visited source "
        "URLs, bounded grounding text, duration, and failures. When visible browsing is "
        "enabled, the separate Revia browser window shows navigation live. Normal query/source "
        "summaries remain in Logs/revia.log; autonomous topic/query/source summaries remain in "
        "RuntimeData/Initiative/curiosity.jsonl. Raw page bodies are intentionally not retained.", this);
    summary->setWordWrap(true);
    summary->setObjectName("secondaryText");
    layout->addWidget(summary);

    clearButton = new QPushButton("Clear view", this);
    clearButton->setToolTip(
        "Clear this table only. Normal summaries remain in Logs/revia.log, autonomous summaries "
        "in RuntimeData/Initiative/curiosity.jsonl, and policy outcomes in Audit/actions.jsonl.");
    clearButton->setMaximumWidth(120);
    layout->addWidget(clearButton, 0, Qt::AlignRight);

    table = new QTableWidget(0, 7, this);
    table->setHorizontalHeaderLabels(
        {"Time", "State", "Initiator", "Backend", "Query", "Sources", "Duration"});
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    layout->addWidget(table, 2);

    detail = new QPlainTextEdit(this);
    detail->setReadOnly(true);
    detail->setMaximumHeight(180);
    detail->setPlaceholderText(
        "Select a lookup to inspect its source URLs, grounding text, or failure detail.");
    layout->addWidget(detail);

    connect(clearButton, &QPushButton::clicked, this, [this]()
    {
        table->setRowCount(0);
        rows.clear();
        detail->clear();
    });
    connect(table, &QTableWidget::itemSelectionChanged,
        this, [this]() { ShowSelectedDetail(); });
    themeListener = revia::desktop::theme::AddListener([this]() { ApplyTones(); });
}

InternetActivityPanel::~InternetActivityPanel()
{
    revia::desktop::theme::RemoveListener(themeListener);
}

void InternetActivityPanel::ApplyTones()
{
    for (int row = 0; row < table->rowCount(); ++row)
    {
        QTableWidgetItem* state = table->item(row, 1);
        if (state != nullptr && state->data(ToneRole).isValid())
        {
            state->setForeground(revia::desktop::theme::RoleColor(
                static_cast<revia::desktop::theme::Role>(state->data(ToneRole).toInt())));
        }
    }
}

void InternetActivityPanel::Observe(const revia::runtime::RuntimeEvent& event)
{
    if (event.kind != revia::runtime::RuntimeEventKind::ComponentStatus ||
        event.component != "Internet activity")
    {
        return;
    }

    const int row = EnsureRow(event);
    SetCell(table, row, 0, EventTime(event));
    SetCell(table, row, 1, QString::fromStdString(event.phase));
    SetCell(table, row, 2, QString::fromStdString(
        event.initiator.empty() ? "Unknown" : event.initiator));
    SetCell(table, row, 3, QString::fromStdString(event.resource));
    SetCell(table, row, 4, QString::fromStdString(event.message));
    SetCell(table, row, 5, QString::number(event.queueDepth));
    SetCell(table, row, 6, event.elapsedMilliseconds >= 0.0
        ? QString::number(event.elapsedMilliseconds, 'f', 1) + QStringLiteral(" ms")
        : QStringLiteral("-"));

    QTableWidgetItem* identity = table->item(row, 0);
    identity->setData(IdentityRole, QString::fromStdString(ActivityIdentity(event)));
    identity->setData(DetailRole, QString::fromStdString(event.detail));
    const revia::desktop::theme::Role tone = PhaseTone(QString::fromStdString(event.phase));
    table->item(row, 1)->setData(ToneRole, static_cast<int>(tone));
    table->item(row, 1)->setForeground(revia::desktop::theme::RoleColor(tone));
    for (int column = 0; column < table->columnCount(); ++column)
    {
        table->item(row, column)->setToolTip(QString::fromStdString(event.detail));
    }
    if (table->currentRow() == row)
    {
        ShowSelectedDetail();
    }
}

int InternetActivityPanel::EnsureRow(const revia::runtime::RuntimeEvent& event)
{
    const std::string identity = ActivityIdentity(event);
    const auto found = rows.find(identity);
    if (found != rows.end()) return found->second;

    if (table->rowCount() >= MaximumVisibleLookups)
    {
        table->removeRow(0);
        RebuildRowIndex();
    }
    const int row = table->rowCount();
    table->insertRow(row);
    rows[identity] = row;
    return row;
}

void InternetActivityPanel::RebuildRowIndex()
{
    rows.clear();
    for (int row = 0; row < table->rowCount(); ++row)
    {
        if (QTableWidgetItem* item = table->item(row, 0))
        {
            rows[item->data(IdentityRole).toString().toStdString()] = row;
        }
    }
}

void InternetActivityPanel::ShowSelectedDetail()
{
    const int row = table->currentRow();
    if (row < 0 || table->item(row, 0) == nullptr)
    {
        detail->clear();
        return;
    }
    detail->setPlainText(table->item(row, 0)->data(DetailRole).toString());
}
