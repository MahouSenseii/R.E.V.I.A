#include "documentsDialog.h"

#include "Actions/actionTypes.h"

#include <QDateTime>
#include <algorithm>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace
{
QString KindLabel(const revia::attachments::AttachmentKind kind)
{
    switch (kind)
    {
        case revia::attachments::AttachmentKind::Image: return QStringLiteral("Image");
        case revia::attachments::AttachmentKind::Pdf: return QStringLiteral("PDF");
        case revia::attachments::AttachmentKind::Docx: return QStringLiteral("Word");
        case revia::attachments::AttachmentKind::Text: return QStringLiteral("Text");
    }
    return QString();
}
} // namespace

DocumentsDialog::DocumentsDialog(DocumentControls inputControls, QWidget* parent) : QDialog(parent), controls(std::move(inputControls))
{
    setObjectName(QStringLiteral("documentsDialog"));
    setWindowTitle(QStringLiteral("Documents"));
    resize(720, 520);
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(QStringLiteral("Files you attached in private chats. They stay on this computer, and Revia searches them when "
                                            "you ask about your documents. Forget deletes the stored copy and everything indexed from it."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    table = new QTableWidget(0, 6, this);
    table->setObjectName(QStringLiteral("documentsTable"));
    table->setHorizontalHeaderLabels(
        {QStringLiteral("Name"), QStringLiteral("Kind"), QStringLiteral("Size"), QStringLiteral("Pages"), QStringLiteral("Indexed"),
            QStringLiteral("Added")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(table, 2);

    auto* buttons = new QHBoxLayout();
    view = new QPushButton(QStringLiteral("View"), this);
    view->setObjectName(QStringLiteral("viewDocumentButton"));
    forget = new QPushButton(QStringLiteral("Forget"), this);
    forget->setObjectName(QStringLiteral("forgetDocumentButton"));
    auto* refresh = new QPushButton(QStringLiteral("Refresh"), this);
    auto* close = new QPushButton(QStringLiteral("Close"), this);
    buttons->addWidget(view);
    buttons->addWidget(forget);
    buttons->addStretch(1);
    buttons->addWidget(refresh);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    image = new QLabel(this);
    image->setAlignment(Qt::AlignCenter);
    image->hide();
    layout->addWidget(image, 1);
    preview = new QPlainTextEdit(this);
    preview->setObjectName(QStringLiteral("documentPreview"));
    preview->setReadOnly(true);
    preview->setPlaceholderText(QStringLiteral("Select a document and choose View to see the start of its text."));
    layout->addWidget(preview, 1);
    status = new QLabel(this);
    status->setObjectName(QStringLiteral("documentsStatus"));
    status->setWordWrap(true);
    layout->addWidget(status);

    connect(table, &QTableWidget::itemSelectionChanged, this, [this]() { UpdateButtons(); });
    connect(table, &QTableWidget::cellDoubleClicked, this, [this](int, int) { ViewSelected(); });
    connect(view, &QPushButton::clicked, this, [this]() { ViewSelected(); });
    connect(forget, &QPushButton::clicked, this, [this]() { ForgetSelected(); });
    connect(refresh, &QPushButton::clicked, this, [this]() { Refresh(); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    Refresh();
}

void DocumentsDialog::Refresh()
{
    const std::string selected = SelectedId();
    documents = controls.list ? controls.list() : std::vector<revia::attachments::StoredDocument>{};
    table->setRowCount(static_cast<int>(documents.size()));
    for (int row = 0; row < static_cast<int>(documents.size()); ++row)
    {
        const auto& document = documents[static_cast<std::size_t>(row)];
        const QString indexed = document.kind == revia::attachments::AttachmentKind::Image
                                    ? QStringLiteral("-")
                                    : QString::number(document.embeddedChunks) + QStringLiteral(" of ") + QString::number(document.chunks);
        const QStringList cells = {QString::fromStdString(document.name), KindLabel(document.kind),
            QString::fromStdString(revia::attachments::HumanSize(document.bytes)),
            document.pages > 0 ? QString::number(document.pages) : QStringLiteral("-"), indexed,
            QDateTime::fromSecsSinceEpoch(document.addedAt).toString(QStringLiteral("yyyy-MM-dd HH:mm"))};
        for (int column = 0; column < cells.size(); ++column)
            table->setItem(row, column, new QTableWidgetItem(cells[column]));
    }
    status->setText(documents.empty() ? QStringLiteral("No documents yet. Attach a file to a private message to add one.")
                                      : QString::number(documents.size()) + QStringLiteral(" kept."));
    Select(selected);
    UpdateButtons();
}

std::string DocumentsDialog::SelectedId() const
{
    const int row = table->currentRow();
    if (row < 0 || row >= static_cast<int>(documents.size()) || table->selectedItems().isEmpty())
        return {};
    return documents[static_cast<std::size_t>(row)].id;
}

void DocumentsDialog::Select(const std::string& id)
{
    for (int row = 0; row < static_cast<int>(documents.size()); ++row)
        if (documents[static_cast<std::size_t>(row)].id == id)
        {
            table->selectRow(row);
            return;
        }
    table->clearSelection();
}

void DocumentsDialog::UpdateButtons()
{
    const bool chosen = !SelectedId().empty();
    view->setEnabled(chosen);
    forget->setEnabled(chosen && static_cast<bool>(controls.forget));
}

void DocumentsDialog::ViewSelected()
{
    const std::string id = SelectedId();
    const auto found = std::find_if(documents.begin(), documents.end(), [&](const auto& document) { return document.id == id; });
    if (found == documents.end())
        return;
    if (found->kind == revia::attachments::AttachmentKind::Image)
    {
        // Decoded by Qt from Revia's own private copy; nothing is opened in another program.
        QImageReader reader(QString::fromStdString(revia::actions::PathToUtf8(found->storedPath)));
        reader.setAllocationLimit(256);
        const QImage picture = reader.read();
        preview->hide();
        image->setPixmap(picture.isNull() ? QPixmap()
                                          : QPixmap::fromImage(picture.scaled(480, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        image->setText(picture.isNull() ? QStringLiteral("The stored picture could not be shown.") : QString());
        image->show();
        return;
    }
    image->hide();
    preview->show();
    preview->setPlainText(controls.preview ? QString::fromStdString(controls.preview(id)) : QString());
}

bool DocumentsDialog::ForgetDocument(const std::string& id, QString& error)
{
    std::string reason;
    if (!controls.forget || !controls.forget(id, reason))
    {
        error = QString::fromStdString(reason.empty() ? std::string("The document could not be forgotten.") : reason);
        status->setText(error);
        return false;
    }
    preview->clear();
    image->clear();
    Refresh();
    status->setText(QStringLiteral("Forgotten: the stored copy, its passages and its search vectors were deleted."));
    return true;
}

void DocumentsDialog::ForgetSelected()
{
    const std::string id = SelectedId();
    const auto found = std::find_if(documents.begin(), documents.end(), [&](const auto& document) { return document.id == id; });
    if (found == documents.end())
        return;
    const auto answer = QMessageBox::question(this, QStringLiteral("Forget document"),
        QStringLiteral("Forget \"") + QString::fromStdString(found->name) +
            QStringLiteral("\"? Its stored copy, passages and search vectors are deleted. This cannot be undone."));
    if (answer != QMessageBox::Yes)
        return;
    QString error;
    (void)ForgetDocument(id, error);
}

int DocumentsDialog::RowCount() const
{
    return table->rowCount();
}

QString DocumentsDialog::PreviewText() const
{
    return preview->toPlainText();
}
