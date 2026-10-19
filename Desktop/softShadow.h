#pragma once

#include <QColor>
#include <QPoint>
#include <QPointer>
#include <QWidget>

class QPainter;
class QPaintEvent;
class QRect;
class QVBoxLayout;

namespace revia::desktop
{
// A soft drop shadow under a rounded rectangle, painted from a cached, blurred nine-slice tile.
// The shell uses this instead of QGraphicsDropShadowEffect, which re-renders a text editor's whole
// subtree offscreen on every caret blink. target is the rectangle that casts the shadow; the
// shadow spreads blur pixels beyond it, shifted by offset.
void PaintSoftShadow(QPainter& painter, const QRect& target, int radius, int blur, QPoint offset, QColor colour);

// Hosts one widget with room around it and paints PaintSoftShadow behind it in the palette's
// Shadow colour. The caller names it (the composer's is composerShadowHost).
class SoftShadowHost final : public QWidget
{
  public:
    explicit SoftShadowHost(QWidget* parent = nullptr);

    // Reparents target into the host; the host follows its geometry and visibility.
    void SetTarget(QWidget* target);
    [[nodiscard]] QWidget* Target() const
    {
        return target;
    }
    void SetRadius(int radius);
    // Also the margin the host keeps around the target, so the shadow is never clipped.
    void SetBlur(int blur);
    void SetOffset(QPoint offset);
    // The colour PaintSoftShadow receives: QPalette::Shadow, softened when the palette carries the
    // opaque default (a theme sets a translucent one).
    [[nodiscard]] QColor ShadowColour() const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void UpdateMargins();

    QVBoxLayout* layout = nullptr;
    QPointer<QWidget> target;
    int radius = 10;
    int blur = 12;
    QPoint offset{0, 2};
};
}
