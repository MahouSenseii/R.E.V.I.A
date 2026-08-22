#pragma once

#include "Runtime/runtimeEvents.h"

#include <QWidget>

#include <vector>

class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QSvgWidget;

// Shows what Revia has drawn, and pictures she was asked to display. Presentation
// only: markup arrives already sanitized and already written to a file, pictures are
// read from a path the runtime already checked, and nothing here can produce or alter one.
class CanvasPanel final : public QWidget
{
public:
    explicit CanvasPanel(QWidget* parent = nullptr);

    void Observe(const revia::runtime::RuntimeEvent& event);

private:
    struct Drawing
    {
        QString title;
        // Empty for a picture loaded from disk; a picture has no markup to check.
        QString markup;
        QString path;
        bool isPicture = false;
    };

    // The canvas is a view of recent work, not the archive; every drawing is already a
    // file on disk. Without a cap the list grows for the life of the session and each
    // entry holds a whole SVG document in memory.
    static constexpr int MaximumDrawings = 50;

    void Show(int index);
    void ExportCurrent();

    // Two views behind a stack rather than one clever widget: an SVG renderer and an
    // image renderer answer to different APIs, and a picture scaled by the wrong one
    // looks broken in a way that reads as a bug in the drawing.
    QStackedWidget* stack = nullptr;
    QSvgWidget* view = nullptr;
    QLabel* pictureView = nullptr;
    QListWidget* list = nullptr;
    QLabel* caption = nullptr;
    QPushButton* exportButton = nullptr;
    std::vector<Drawing> drawings;
};
