#include "canvasPanel.h"

#include <QAbstractItemView>
#include <QByteArray>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QPixmap>
#include <QStackedWidget>
#include <QSvgWidget>
#include <QVBoxLayout>

#include <filesystem>
#include <fstream>

CanvasPanel::CanvasPanel(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 14, 12, 16);
    layout->setSpacing(10);

    auto* intro = new QLabel(
        "Drawings Revia has made, and pictures you asked her to show. Ask for a diagram "
        "or a mockup in conversation and it lands here; /show <path> displays an image "
        "from an approved folder. Every drawing is SVG that was checked before it was "
        "rendered: no script, no external references, no embedded documents. One that "
        "wanted any of those was refused rather than cleaned up.", this);
    intro->setWordWrap(true);
    intro->setObjectName("secondaryText");
    layout->addWidget(intro);

    auto* split = new QHBoxLayout();
    split->setSpacing(10);
    layout->addLayout(split, 1);

    list = new QListWidget(this);
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
    pictureView->setAlignment(Qt::AlignCenter);
    // Scaled contents would stretch a photo to the panel's aspect ratio. The pixmap is
    // resized on show instead, keeping the picture's own proportions.
    pictureView->setScaledContents(false);
    stack->addWidget(view);
    stack->addWidget(pictureView);
    right->addWidget(stack, 1);

    caption = new QLabel(
        "Nothing here yet. Try asking: draw me a diagram of the turn path.", this);
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
        const QPixmap picture(drawing.path);
        if (picture.isNull())
        {
            caption->setText("Could not read " + drawing.path);
            return;
        }
        pictureView->setPixmap(picture.scaled(
            stack->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        stack->setCurrentWidget(pictureView);
    }
    else
    {
        view->load(QByteArray(drawing.markup.toUtf8()));
        stack->setCurrentWidget(view);
    }
    caption->setText(drawing.title + "\n" + drawing.path);
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
    const Drawing& drawing = drawings[static_cast<std::size_t>(index)];
    const QString target = QFileDialog::getSaveFileName(
        this, "Save diagram", drawing.title + ".svg", "SVG image (*.svg)");
    if (target.isEmpty())
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
    caption->setText(file.good()
        ? "Saved a copy to " + target
        : "The copy could not be written completely.");
}

void CanvasPanel::Observe(const revia::runtime::RuntimeEvent& event)
{
    if (event.kind != revia::runtime::RuntimeEventKind::Diagram)
    {
        return;
    }
    const bool isPicture = event.phase == "Image";
    // A drawing needs its markup; a picture needs its path. Neither is useful without.
    if ((isPicture && event.resource.empty()) || (!isPicture && event.detail.empty()))
    {
        return;
    }
    Drawing drawing;
    drawing.title = QString::fromStdString(event.message);
    drawing.markup = QString::fromStdString(event.detail);
    drawing.path = QString::fromStdString(event.resource);
    drawing.isPicture = isPicture;

    // Newest first, and selected immediately: a drawing the user just asked for should be
    // on screen when they switch to the tab, not one click away.
    drawings.insert(drawings.begin(), std::move(drawing));
    list->insertItem(0, drawings.front().title);

    // Oldest out. The file stays where it was written, so nothing is lost except a copy
    // of markup that was only being held to redraw a thumbnail nobody scrolled back to.
    while (static_cast<int>(drawings.size()) > MaximumDrawings)
    {
        drawings.pop_back();
        delete list->takeItem(list->count() - 1);
    }
    list->setCurrentRow(0);
}
