#include "canvasPanel.h"

#include <QAbstractItemView>
#include <QByteArray>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QPixmap>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QSvgWidget>
#include <QVBoxLayout>

#include <filesystem>
#include <fstream>
#include <algorithm>

CanvasPanel::CanvasPanel(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 14, 12, 16);
    layout->setSpacing(10);

    auto* intro = new QLabel("Drawings made in conversation, and pictures you asked to see. Ask for a diagram "
                             "or a mockup in conversation and it lands here; /show <path> displays an image "
                             "from an approved folder. Every drawing is SVG that was checked before it was "
                             "rendered: no script, no external references, no embedded documents. One that "
                             "wanted any of those was refused rather than cleaned up.",
        this);
    intro->setWordWrap(true);
    intro->setObjectName("secondaryText");
    layout->addWidget(intro);

    auto* split = new QHBoxLayout();
    split->setSpacing(10);
    layout->addLayout(split, 1);

    list = new QListWidget(this);
    list->setObjectName("canvasDrawingList");
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setMaximumWidth(240);
    split->addWidget(list);

    auto* right = new QVBoxLayout();
    right->setSpacing(8);
    split->addLayout(right, 1);

    stack = new QStackedWidget(this);
    stack->setMinimumHeight(320);
    view = new QSvgWidget(stack);
    pictureView = new QLabel(stack);
    pictureView->setObjectName("canvasImagePreview");
    pictureView->setAlignment(Qt::AlignCenter);
    // Scaled contents would stretch a photo to the panel's aspect ratio. The pixmap is
    // resized on show instead, keeping the picture's own proportions.
    pictureView->setScaledContents(false);
    stack->addWidget(view);
    stack->addWidget(pictureView);
    right->addWidget(stack, 1);

    caption = new QLabel("Nothing here yet. Try asking: draw me a diagram of the turn path.", this);
    caption->setWordWrap(true);
    caption->setObjectName("secondaryText");
    right->addWidget(caption);

    exportButton = new QPushButton("Save a copy...", this);
    exportButton->setEnabled(false);
    right->addWidget(exportButton, 0, Qt::AlignLeft);

    connect(list, &QListWidget::currentRowChanged, this, &CanvasPanel::Show);
    connect(exportButton, &QPushButton::clicked, this, &CanvasPanel::ExportCurrent);
}

void CanvasPanel::Show(const int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= drawings.size())
    {
        return;
    }
    const Drawing& drawing = drawings[static_cast<std::size_t>(index)];
    if (drawing.isPicture)
    {
        pictureView->clear();
        QPixmap picture;
        if (drawing.reference)
        {
            const auto image = Resolve(*drawing.reference);
            if (!image)
            {
                RemoveMedia({drawing.reference->id});
                return;
            }
            picture = QPixmap::fromImage(*image);
        }
        else
            picture.load(drawing.path);
        if (picture.isNull())
        {
            caption->setText("Could not read " + drawing.path);
            return;
        }
        pictureView->setPixmap(picture.scaled(stack->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        stack->setCurrentWidget(pictureView);
    }
    else
    {
        pictureView->clear();
        view->load(QByteArray(drawing.markup.toUtf8()));
        stack->setCurrentWidget(view);
    }
    caption->setText(drawing.title + "\n" +
                     (drawing.reference ? QStringLiteral("Generated: ") + QString::fromStdString(drawing.reference->name) : drawing.path));
    // A picture already belongs to the user; only a drawing Revia produced is worth
    // offering to save somewhere else.
    exportButton->setEnabled(!drawing.isPicture);
}

void CanvasPanel::ExportCurrent()
{
    const int index = list->currentRow();
    if (index < 0 || static_cast<std::size_t>(index) >= drawings.size())
    {
        return;
    }
    const Drawing drawing = drawings[static_cast<std::size_t>(index)];
    const auto revision = presentationRevision;
    const QString target = QFileDialog::getSaveFileName(this, "Save diagram", drawing.title + ".svg", "SVG image (*.svg)");
    if (target.isEmpty() || revision != presentationRevision)
    {
        return;
    }
    const std::filesystem::path targetPath(target.toStdWString());
    std::ofstream file(targetPath, std::ios::trunc | std::ios::binary);
    if (!file.is_open())
    {
        caption->setText("Could not write " + target);
        return;
    }
    const QByteArray markup = drawing.markup.toUtf8();
    file.write(markup.constData(), markup.size());
    caption->setText(file.good() ? "Saved a copy to " + target : "The copy could not be written completely.");
}

void CanvasPanel::Observe(const revia::runtime::RuntimeEvent& event)
{
    if (event.kind != revia::runtime::RuntimeEventKind::Diagram)
    {
        return;
    }
    const bool isPicture = event.phase == "Image" || event.generatedMediaReference.has_value();
    if (event.generatedMediaReference && !event.generatedMediaReference->mimeType.starts_with("image/"))
        return;
    if (event.generatedMediaReference && !Resolve(*event.generatedMediaReference))
        return;
    if ((isPicture && !event.generatedMediaReference && event.resource.empty()) || (!isPicture && event.detail.empty()))
    {
        return;
    }
    Drawing drawing;
    drawing.title = QString::fromStdString(event.message);
    drawing.markup = QString::fromStdString(event.detail);
    drawing.path = event.generatedMediaReference ? QString() : QString::fromStdString(event.resource);
    drawing.isPicture = isPicture;
    drawing.reference = event.generatedMediaReference;

    // Newest first, and selected immediately: a drawing the user just asked for should be
    // on screen when they switch to the tab, not one click away.
    ++presentationRevision;
    drawings.insert(drawings.begin(), std::move(drawing));
    const QSignalBlocker blocked(list);
    list->insertItem(0, drawings.front().title);

    // Oldest out. The file stays where it was written, so nothing is lost except a copy
    // of markup that was only being held to redraw a thumbnail nobody scrolled back to.
    while (static_cast<int>(drawings.size()) > MaximumDrawings)
    {
        drawings.pop_back();
        delete list->takeItem(list->count() - 1);
    }
    list->setCurrentRow(0);
    Show(0);
}

void CanvasPanel::SetGeneratedImageResolver(GeneratedImageResolver resolver)
{
    generatedImageResolver = std::move(resolver);
}

std::optional<QImage> CanvasPanel::Resolve(const conversationMediaReference& reference) const
{
    if (reference.origin != MediaOrigin::GeneratedArtifact || reference.id.empty() || reference.digest.empty() || !generatedImageResolver)
        return {};
    try
    {
        auto image = generatedImageResolver(reference);
        return image && !image->isNull() ? std::move(image) : std::nullopt;
    }
    catch (...)
    {
        return {};
    }
}

std::vector<conversationMediaReference> CanvasPanel::GeneratedMediaReferences() const
{
    std::vector<conversationMediaReference> references;
    for (const auto& drawing : drawings)
        if (drawing.reference)
            references.push_back(*drawing.reference);
    return references;
}

void CanvasPanel::ClearView()
{
    pictureView->clear();
    const int index = stack->indexOf(view);
    stack->removeWidget(view);
    delete view;
    view = new QSvgWidget(stack);
    stack->insertWidget(index, view);
    stack->setCurrentWidget(view);
    exportButton->setEnabled(false);
    caption->setText(QStringLiteral("Nothing here yet. Try asking: draw me a diagram of the turn path."));
}

void CanvasPanel::RemoveMedia(const std::set<std::string>& ids)
{
    if (std::none_of(drawings.begin(), drawings.end(),
            [&](const Drawing& drawing) { return drawing.reference && ids.contains(drawing.reference->id); }))
        return;
    ++presentationRevision;
    const QSignalBlocker blocked(list);
    drawings.erase(std::remove_if(drawings.begin(), drawings.end(),
                       [&](const Drawing& drawing) { return drawing.reference && ids.contains(drawing.reference->id); }),
        drawings.end());
    list->clear();
    for (const auto& drawing : drawings)
        list->addItem(drawing.title);
    ClearView();
    if (!drawings.empty())
    {
        list->setCurrentRow(0);
        Show(0);
    }
}

void CanvasPanel::Clear()
{
    ++presentationRevision;
    const QSignalBlocker blocked(list);
    drawings.clear();
    list->clear();
    ClearView();
}
