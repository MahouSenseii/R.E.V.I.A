#include "memoryPanel.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace
{
    // createdAt is stored as a Unix epoch. Shown raw it is a ten-digit number, which
    // tells a reader nothing about whether Revia learned something today or last month.
    QString RememberedAt(const std::string& createdAt)
    {
        const QString raw = QString::fromStdString(createdAt);
        bool numeric = false;
        const qlonglong seconds = raw.toLongLong(&numeric);
        if (!numeric || seconds <= 0)
        {
            // Already a formatted timestamp, or something unexpected. Either way it is
            // the store's own text and is shown as written rather than guessed at.
            return raw;
        }
        return QDateTime::fromSecsSinceEpoch(seconds).toString("yyyy-MM-dd HH:mm");
    }

    QString ImportanceLabel(const memoryImportance importance)
    {
        switch (importance)
        {
            case memoryImportance::High: return QStringLiteral("High");
            case memoryImportance::Low: return QStringLiteral("Low");
            case memoryImportance::Medium: break;
        }
        return QStringLiteral("Medium");
    }
}

MemoryPanel::MemoryPanel(
    revia::runtime::ReviaSession& inputSession,
    QWidget* parent)
    : QWidget(parent),
      session(inputSession)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(10);

    auto* title = new QLabel("Memory", this);
    title->setObjectName("sectionTitle");
    layout->addWidget(title);

    auto* explanation = new QLabel(
        "Everything Revia has kept, and nothing she has not. Entries are written by the "
        "reviewed memory path, so this is a window rather than an editor: what you see "
        "here is exactly what reaches her prompt when something is relevant.", this);
    explanation->setWordWrap(true);
    explanation->setObjectName("secondaryText");
    layout->addWidget(explanation);

    statusLabel = new QLabel(this);
    statusLabel->setObjectName("memoryStatus");
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto* controls = new QHBoxLayout();
    searchInput = new QLineEdit(this);
    searchInput->setPlaceholderText("Search remembered facts...");
    searchInput->setClearButtonEnabled(true);
    // A search field spanning the whole monitor looks like a text editor, and the cursor
    // ends up nowhere near the results it filters.
    // Bounded at both ends: without a floor it collapses to its size hint and shows
    // about two words of the placeholder.
    searchInput->setMinimumWidth(260);
    searchInput->setMaximumWidth(520);
    controls->addWidget(searchInput);
    controls->addStretch(1);
    highImportanceOnly = new QCheckBox("High importance only", this);
    controls->addWidget(highImportanceOnly);
    refreshButton = new QPushButton("Refresh", this);
    controls->addWidget(refreshButton);
    layout->addLayout(controls);

    table = new QTableWidget(this);
    table->setColumnCount(5);
    table->setHorizontalHeaderLabels(
        {"Summary", "Category", "Importance", "Source", "Remembered"});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 5; ++column)
    {
        table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    // One line per memory, elided, with the full text on hover.
    //
    // Wrapping plus resizeRowsToContents produced rows around 140px tall for a single
    // sentence, so four memories filled a 1080p screen and the table could not be
    // scanned at all. A memory list is read by sweeping down it looking for one thing,
    // which wants uniform compact rows, not paragraphs.
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideRight);
    table->verticalHeader()->setDefaultSectionSize(30);
    layout->addWidget(table, 1);

    connect(searchInput, &QLineEdit::textChanged, this, [this](const QString&)
    {
        ApplyFilter();
    });
    connect(highImportanceOnly, &QCheckBox::toggled, this, [this](bool)
    {
        ApplyFilter();
    });
    connect(refreshButton, &QPushButton::clicked, this, [this]() { Refresh(); });

    // Populated immediately rather than waiting for the runtime to finish starting. An
    // empty table with no status line is indistinguishable from a broken panel, and the
    // store is a file on disk that does not need the session to be up to be read.
    Refresh();
}

void MemoryPanel::Refresh()
{
    statusLabel->setText(QString::fromStdString(session.MemoryStatus()));
    ApplyFilter();
}

void MemoryPanel::ApplyFilter()
{
    const std::string query = searchInput->text().trimmed().toStdString();
    // An empty query lists everything; a non-empty one goes through the store's own
    // ranked search, so the order here is the order Revia would actually retrieve in.
    std::vector<memoryEntry> entries = session.SearchMemories(query, 200);
    if (highImportanceOnly->isChecked())
    {
        std::vector<memoryEntry> filtered;
        filtered.reserve(entries.size());
        for (memoryEntry& entry : entries)
        {
            if (entry.importance == memoryImportance::High)
            {
                filtered.push_back(std::move(entry));
            }
        }
        entries = std::move(filtered);
    }
    Render(entries);
}

void MemoryPanel::Render(const std::vector<memoryEntry>& entries)
{
    table->setRowCount(static_cast<int>(entries.size()));
    for (int row = 0; row < static_cast<int>(entries.size()); ++row)
    {
        const memoryEntry& entry = entries[static_cast<std::size_t>(row)];
        const auto cell = [](const std::string& value)
        {
            auto* item = new QTableWidgetItem(QString::fromStdString(value));
            // Elision hides the tail of a long memory, so the whole of it has to remain
            // reachable somewhere. Hovering is that somewhere.
            item->setToolTip(QString::fromStdString(value));
            return item;
        };
        table->setItem(row, 0, cell(entry.summary));
        table->setItem(row, 1, cell(entry.category));
        table->setItem(row, 2, new QTableWidgetItem(ImportanceLabel(entry.importance)));
        table->setItem(row, 3, cell(entry.source));
        table->setItem(row, 4, new QTableWidgetItem(RememberedAt(entry.createdAt)));
    }
    if (entries.empty())
    {
        // An empty table and an empty memory look identical, and the difference matters:
        // one means the search found nothing, the other means she has kept nothing yet.
        const bool searching = !searchInput->text().trimmed().isEmpty() ||
            highImportanceOnly->isChecked();
        statusLabel->setText(searching
            ? QStringLiteral("Nothing stored matches that.")
            : QString::fromStdString(session.MemoryStatus()));
    }
}
