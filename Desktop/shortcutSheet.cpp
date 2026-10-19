#include "shortcutSheet.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFont>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <cstddef>

namespace revia::desktop
{
ShortcutSheet::ShortcutSheet(std::vector<std::pair<QString, QString>> rows, QWidget* parent) : QDialog(parent)
{
    setObjectName("shortcutsDialog");
    setWindowTitle("Keyboard shortcuts");
    setModal(false);
    setAttribute(Qt::WA_QuitOnClose, false);
    resize(440, 520);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(12);

    auto* table = new QTableWidget(static_cast<int>(rows.size()), 2, this);
    table->setObjectName("shortcutsTable");
    table->setHorizontalHeaderLabels({"Action", "Shortcut"});
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setHighlightSections(false);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->setFocusPolicy(Qt::StrongFocus);
    for (int row = 0; row < static_cast<int>(rows.size()); ++row)
    {
        const auto& [action, keys] = rows[static_cast<std::size_t>(row)];
        auto* actionItem = new QTableWidgetItem(action);
        if (keys.isEmpty())
        {
            // A section heading across both columns.
            QFont font = actionItem->font();
            font.setWeight(QFont::DemiBold);
            actionItem->setFont(font);
            actionItem->setFlags(Qt::ItemIsEnabled);
            table->setItem(row, 0, actionItem);
            table->setSpan(row, 0, 1, 2);
            continue;
        }
        table->setItem(row, 0, actionItem);
        auto* keysItem = new QTableWidgetItem(keys);
        keysItem->setData(Qt::AccessibleTextRole, action + QStringLiteral(": ") + keys);
        table->setItem(row, 1, keysItem);
    }
    table->resizeRowsToContents();
    layout->addWidget(table, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
    // Opening on Close keeps the table from highlighting its first heading; Tab reaches the table.
    QPushButton* close = buttons->button(QDialogButtonBox::Close);
    close->setDefault(true);
    close->setFocus();
}
}
