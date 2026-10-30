#pragma once

#include "Runtime/runtimeEvents.h"

#include <QWidget>
#include <QImage>

#include <functional>
#include <optional>
#include <set>
#include <vector>

class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QSvgWidget;

// Shows what Revia has drawn, and pictures she was asked to display. Presentation
// only: markup arrives already sanitized, generated pixels need a current host resolver,
// and other pictures use paths the runtime checked. This widget cannot produce or alter media.
class CanvasPanel final : public QWidget
{
  public:
    explicit CanvasPanel(QWidget* parent = nullptr);

    void Observe(const revia::runtime::RuntimeEvent& event);
    using GeneratedImageResolver = std::function<std::optional<QImage>(const conversationMediaReference&)>;
    void SetGeneratedImageResolver(GeneratedImageResolver resolver);
    [[nodiscard]] std::vector<conversationMediaReference> GeneratedMediaReferences() const;
    void RemoveMedia(const std::set<std::string>& ids);
    void Clear();

  private:
    struct Drawing
    {
        QString title;
        // Empty for a picture loaded from disk; a picture has no markup to check.
        QString markup;
        QString path;
        bool isPicture = false;
        std::optional<conversationMediaReference> reference;
    };

    // The canvas is a view of recent work, not the archive; every drawing is already a
    // file on disk. Without a cap the list grows for the life of the session and each
    // entry holds a whole SVG document in memory.
    static constexpr int MaximumDrawings = 50;

    void Show(int index);
    void ExportCurrent();
    void ClearView();
    [[nodiscard]] std::optional<QImage> Resolve(const conversationMediaReference& reference) const;

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
    GeneratedImageResolver generatedImageResolver;
    std::uint64_t presentationRevision = 0;
};
