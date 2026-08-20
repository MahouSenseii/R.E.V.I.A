#include "resourcePanel.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace
{
    void ConfigureTable(QTableWidget* table)
    {
        table->verticalHeader()->setVisible(false);
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true);
        table->setShowGrid(false);
    }

    QString MemoryLabel(const revia::runtime::RuntimeEvent& event)
    {
        if (event.totalMemoryMiB == 0)
        {
            return QStringLiteral("-");
        }
        return QString::number(event.availableMemoryMiB) + QStringLiteral(" / ") +
            QString::number(event.totalMemoryMiB) + QStringLiteral(" MiB free / total");
    }
}

ResourcePanel::ResourcePanel(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(8);

    auto* summary = new QLabel(
        "Revia rebuilds this plan on every startup. Independent long-lived workers are "
        "placed on different devices when possible; RAM is a bounded cache and model "
        "mapping layer, not a substitute for GPU memory.", this);
    summary->setWordWrap(true);
    summary->setObjectName("secondaryText");
    layout->addWidget(summary);

    auto* hardwareTitle = new QLabel("Detected hardware and budgets", this);
    hardwareTitle->setObjectName("sectionTitle");
    layout->addWidget(hardwareTitle);
    hardwareTable = new QTableWidget(0, 5, this);
    hardwareTable->setHorizontalHeaderLabels(
        {"Resource", "Identity", "Startup capacity", "Configured ceiling", "Detail"});
    ConfigureTable(hardwareTable);
    hardwareTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    hardwareTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    hardwareTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    hardwareTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    hardwareTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    hardwareTable->setMinimumHeight(150);
    layout->addWidget(hardwareTable, 1);

    auto* assignmentTitle = new QLabel("Resolved workload assignments", this);
    assignmentTitle->setObjectName("sectionTitle");
    layout->addWidget(assignmentTitle);
    assignmentTable = new QTableWidget(0, 3, this);
    assignmentTable->setHorizontalHeaderLabels({"Workload", "Planned device", "Strategy"});
    ConfigureTable(assignmentTable);
    assignmentTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    assignmentTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    assignmentTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    assignmentTable->setMinimumHeight(150);
    layout->addWidget(assignmentTable, 1);
}

int ResourcePanel::EnsureHardwareRow(const QString& name)
{
    const auto found = hardwareRows.find(name);
    if (found != hardwareRows.end())
    {
        return found->second;
    }
    const int row = hardwareTable->rowCount();
    hardwareTable->insertRow(row);
    hardwareRows.emplace(name, row);
    SetCell(hardwareTable, row, 0, name);
    return row;
}

int ResourcePanel::EnsureAssignmentRow(const QString& workload)
{
    const auto found = assignmentRows.find(workload);
    if (found != assignmentRows.end())
    {
        return found->second;
    }
    const int row = assignmentTable->rowCount();
    assignmentTable->insertRow(row);
    assignmentRows.emplace(workload, row);
    SetCell(assignmentTable, row, 0, workload);
    return row;
}

void ResourcePanel::SetCell(
    QTableWidget* table,
    const int row,
    const int column,
    const QString& text)
{
    QTableWidgetItem* item = table->item(row, column);
    if (item == nullptr)
    {
        item = new QTableWidgetItem();
        table->setItem(row, column, item);
    }
    item->setText(text);
}

void ResourcePanel::Observe(const revia::runtime::RuntimeEvent& event)
{
    if (event.kind != revia::runtime::RuntimeEventKind::ResourceStatus ||
        event.component.empty())
    {
        return;
    }
    const QString component = QString::fromStdString(event.component);
    if (event.phase == "Assignment")
    {
        const int row = EnsureAssignmentRow(component);
        SetCell(assignmentTable, row, 1, QString::fromStdString(event.resource));
        SetCell(assignmentTable, row, 2, QString::fromStdString(event.message));
        return;
    }

    const int row = EnsureHardwareRow(component);
    SetCell(hardwareTable, row, 1, QString::fromStdString(event.resource));
    SetCell(hardwareTable, row, 2, MemoryLabel(event));
    SetCell(hardwareTable, row, 3, event.allocatedMemoryMiB > 0
        ? QString::number(event.allocatedMemoryMiB) + QStringLiteral(" MiB")
        : QStringLiteral("role budget"));
    SetCell(hardwareTable, row, 4, QString::fromStdString(event.message));
}
