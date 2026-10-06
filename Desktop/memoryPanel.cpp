#include "Memory/memoryTypes.h"
#include "memoryPanel.h"
#include "Audit/contentDigest.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QUuid>

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
    case memoryImportance::High:
        return QStringLiteral("High");
    case memoryImportance::Low:
        return QStringLiteral("Low");
    case memoryImportance::Medium:
        break;
    }
    return QStringLiteral("Medium");
}
}

MemoryPanel::MemoryPanel(revia::runtime::ReviaSession& inputSession, QWidget* parent) : QWidget(parent), session(inputSession)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(10);

    auto* title = new QLabel("Memory", this);
    title->setObjectName("sectionTitle");
    layout->addWidget(title);

    auto* explanation =
        new QLabel("Stored facts retain their source and revision history. Select a current record to request an exact correction. "
                   "The original remains available; retrieval decides which facts are relevant.",
            this);
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
    table->setObjectName("memoryRecords");
    table->setMinimumHeight(130);
    table->setColumnCount(6);
    table->setHorizontalHeaderLabels({"Summary", "Category", "Importance", "Source", "Remembered", "Revision"});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 6; ++column)
    {
        table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
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
    auto* revisionRow = new QHBoxLayout();
    revisionProvenance = new QLabel(this);
    revisionProvenance->setObjectName("memoryRevisionProvenance");
    revisionProvenance->setWordWrap(true);
    revisionProvenance->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    revisionProvenance->setOpenExternalLinks(false);
    revisionRow->addWidget(revisionProvenance, 1);
    reviseButton = new QPushButton("Correct selected record", this);
    reviseButton->setObjectName("memoryReviseSelected");
    reviseButton->setEnabled(false);
    revisionRow->addWidget(reviseButton);
    layout->addLayout(revisionRow);
    revisionOutcome = new QLabel(this);
    revisionOutcome->setObjectName("memoryRevisionOutcome");
    revisionOutcome->setTextFormat(Qt::PlainText);
    revisionOutcome->setWordWrap(true);
    revisionOutcome->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(revisionOutcome);

    connect(searchInput, &QLineEdit::textChanged, this, [this](const QString&) { ApplyFilter(); });
    connect(highImportanceOnly, &QCheckBox::toggled, this, [this](bool) { ApplyFilter(); });
    connect(refreshButton, &QPushButton::clicked, this, [this]() { Refresh(); });
    connect(table, &QTableWidget::itemSelectionChanged, this, [this]() { UpdateSelectedRevision(); });
    connect(reviseButton, &QPushButton::clicked, this, [this]() { ReviseSelectedMemory(); });
    connect(revisionProvenance, &QLabel::linkActivated, this, [this](const QString& link) { SelectLinkedMemory(link); });

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
    const auto previousId = table->currentRow() >= 0 && table->currentRow() < static_cast<int>(displayedEntries.size())
                                ? displayedEntries[static_cast<std::size_t>(table->currentRow())].id
                                : std::string();
    const QSignalBlocker block(table);
    displayedEntries = entries;
    memorySnapshotOrigin = session.Stamp();
    memorySnapshotAudienceRevision = session.Audience().revision;
    table->clearSelection();
    table->setCurrentCell(-1, -1);
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
        const bool historical = !entry.currentRevisionId.empty() && entry.currentRevisionId != entry.id;
        auto* revision = new QTableWidgetItem(historical ? "Historical" : "Current");
        revision->setToolTip(
            QString::fromStdString("Record: " + entry.id + "\nChain: " + entry.revisionChainId + "\nReceipt: " + entry.revisionReceiptId +
                                   "\nRevises: " + entry.revisesMemoryId + "\nCurrent: " + entry.currentRevisionId));
        table->setItem(row, 5, revision);
        if (!previousId.empty() && entry.id == previousId)
            table->selectRow(row);
    }
    if (entries.empty())
    {
        // An empty table and an empty memory look identical, and the difference matters:
        // one means the search found nothing, the other means she has kept nothing yet.
        const bool searching = !searchInput->text().trimmed().isEmpty() || highImportanceOnly->isChecked();
        statusLabel->setText(searching ? QStringLiteral("Nothing stored matches that.") : QString::fromStdString(session.MemoryStatus()));
    }
    UpdateSelectedRevision();
}

void MemoryPanel::SetRevisionSubmitter(std::function<bool(const revia::memory::MemoryRevisionRequest&)> submitter)
{
    submitRevision = std::move(submitter);
    UpdateSelectedRevision();
}

void MemoryPanel::SetRevisionBusy(const bool busy)
{
    revisionBusy = busy;
    if (revisionDialog)
    {
        if (auto* button = revisionDialog->findChild<QPushButton*>("memoryRevisionRecord"))
            button->setEnabled(!busy);
    }
    UpdateSelectedRevision();
}

void MemoryPanel::SetRevisionOutcome(const bool success, const std::string& message)
{
    revisionOutcome->setText(QString::fromStdString(message));
    revisionOutcome->setProperty("error", !success);
    if (revisionDialog)
    {
        if (auto* result = revisionDialog->findChild<QLabel*>("memoryRevisionDialogOutcome"))
            result->setText(QString::fromStdString(message));
        if (success)
        {
            if (auto* button = revisionDialog->findChild<QPushButton*>("memoryRevisionRecord"))
                button->setEnabled(false);
        }
    }
}

void MemoryPanel::UpdateSelectedRevision()
{
    const int row = table->currentRow();
    if (row < 0 || row >= static_cast<int>(displayedEntries.size()))
    {
        revisionProvenance->setText("Select a record to inspect its exact revision links.");
        reviseButton->setEnabled(false);
        return;
    }
    const auto& entry = displayedEntries[static_cast<std::size_t>(row)];
    const bool historical = !entry.currentRevisionId.empty() && entry.currentRevisionId != entry.id;
    QString links = "Record " + QString::fromStdString(entry.id).toHtmlEscaped();
    if (!entry.revisionChainId.empty())
        links += " · Chain " + QString::fromStdString(entry.revisionChainId).toHtmlEscaped();
    if (!entry.revisionReceiptId.empty())
        links += "<br>Receipt " + QString::fromStdString(entry.revisionReceiptId).toHtmlEscaped();
    if (!entry.revisesMemoryId.empty())
        links += " · <a href=\"prior\">Prior record</a> " + QString::fromStdString(entry.revisesMemoryId).toHtmlEscaped();
    if (historical)
        links += "<br>Historical · <a href=\"current\">Select current replacement</a> " +
                 QString::fromStdString(entry.currentRevisionId).toHtmlEscaped();
    revisionProvenance->setText(links);
    reviseButton->setEnabled(!revisionBusy && !historical && !entry.id.empty() && static_cast<bool>(submitRevision));
}

void MemoryPanel::SelectLinkedMemory(const QString& link)
{
    const int row = table->currentRow();
    if (row < 0 || row >= static_cast<int>(displayedEntries.size()))
        return;
    const auto& entry = displayedEntries[static_cast<std::size_t>(row)];
    const auto id = link == "current" ? entry.currentRevisionId : (link == "prior" ? entry.revisesMemoryId : std::string());
    if (id.empty())
        return;
    searchInput->clear();
    highImportanceOnly->setChecked(false);
    Render(session.Memories());
    for (int index = 0; index < static_cast<int>(displayedEntries.size()); ++index)
    {
        if (displayedEntries[static_cast<std::size_t>(index)].id == id)
        {
            table->selectRow(index);
            table->scrollToItem(table->item(index, 0));
            return;
        }
    }
}

void MemoryPanel::ReviseSelectedMemory()
{
    const int row = table->currentRow();
    if (revisionBusy || !submitRevision || row < 0 || row >= static_cast<int>(displayedEntries.size()))
        return;
    const auto entry = displayedEntries[static_cast<std::size_t>(row)];
    if (!entry.currentRevisionId.empty() && entry.currentRevisionId != entry.id)
        return;
    if (revisionDialog)
    {
        revisionDialog->raise();
        return;
    }
    revia::memory::MemoryRevisionRequest request;
    request.ownerRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    request.originalId = entry.id;
    request.expectedSummaryDigest = revia::audit::ContentDigest(entry.summary);
    request.expectedSubject = entry.subject;
    request.priorReceiptId = entry.revisionReceiptId;
    request.origin = memorySnapshotOrigin;
    request.audienceRevision = memorySnapshotAudienceRevision;
    request.corrected.bSuccess = true;
    request.corrected.bShouldRemember = true;
    request.corrected.category = entry.category;
    request.corrected.source = "owner_revision";
    request.corrected.subject = entry.subject;
    revisionDialog = new QDialog(this);
    revisionDialog->setObjectName("memoryRevisionDialog");
    revisionDialog->setWindowTitle("Correct selected memory");
    revisionDialog->setAttribute(Qt::WA_DeleteOnClose);
    revisionDialog->resize(540, 480);
    auto* layout = new QVBoxLayout(revisionDialog);
    auto* explanation =
        new QLabel("Request a correction to this exact current record. Its original text remains in history.", revisionDialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* original = new QPlainTextEdit(revisionDialog);
    original->setObjectName("memoryRevisionOriginal");
    original->setReadOnly(true);
    original->setPlainText(QString::fromStdString(entry.summary));
    original->setMaximumHeight(80);
    layout->addWidget(original);
    auto* corrected = new QPlainTextEdit(revisionDialog);
    corrected->setObjectName("memoryRevisionCorrected");
    corrected->setPlaceholderText("Write the corrected fact");
    corrected->setMaximumHeight(110);
    layout->addWidget(corrected);
    auto* reason = new QLineEdit(revisionDialog);
    reason->setObjectName("memoryRevisionReason");
    reason->setMaxLength(1024);
    reason->setPlaceholderText("Why should this exact record be corrected?");
    layout->addWidget(reason);
    auto* evidence = new QPlainTextEdit(revisionDialog);
    evidence->setObjectName("memoryRevisionEvidence");
    evidence->setPlaceholderText("Your explicit correction evidence");
    evidence->setMaximumHeight(90);
    layout->addWidget(evidence);
    auto* result = new QLabel(revisionDialog);
    result->setObjectName("memoryRevisionDialogOutcome");
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(result);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, revisionDialog);
    auto* record = buttons->addButton("Request exact correction", QDialogButtonBox::ActionRole);
    record->setObjectName("memoryRevisionRecord");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, revisionDialog, &QDialog::close);
    connect(record, &QPushButton::clicked, this,
        [this, request, corrected, reason, evidence]() mutable
        {
            request.corrected.summary = corrected->toPlainText().trimmed().toStdString();
            request.reason = reason->text().trimmed().toStdString();
            request.evidence = evidence->toPlainText().trimmed().toStdString();
            if (request.corrected.summary.empty() || request.corrected.summary.size() > 4096 || request.reason.empty() ||
                request.reason.size() > 1024 || request.evidence.empty() || request.evidence.size() > 2048)
            {
                SetRevisionOutcome(false, "Supply a corrected fact (up to 4096 bytes), reason (1024 bytes) and evidence (2048 bytes).");
                return;
            }
            if (!submitRevision(request))
                SetRevisionOutcome(false, "Another companion operation is active. Request the correction after it finishes.");
        });
    revisionDialog->show();
}
