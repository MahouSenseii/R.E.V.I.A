#include "softShadow.h"

#include <QEvent>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QRect>
#include <QRectF>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <map>
#include <tuple>
#include <vector>

namespace revia::desktop
{
namespace
{
// One box-blur pass over a square alpha buffer, horizontally then vertically.
void BoxBlur(std::vector<float>& alpha, const int side, const int radius)
{
    if (radius <= 0)
        return;
    std::vector<float> pass(alpha.size(), 0.0f);
    const float window = static_cast<float>(2 * radius + 1);
    const auto at = [side](const int x, const int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(side) + x; };
    for (int y = 0; y < side; ++y)
    {
        for (int x = 0; x < side; ++x)
        {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k)
            {
                const int sample = x + k;
                if (sample >= 0 && sample < side)
                    sum += alpha[at(sample, y)];
            }
            pass[at(x, y)] = sum / window;
        }
    }
    for (int y = 0; y < side; ++y)
    {
        for (int x = 0; x < side; ++x)
        {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k)
            {
                const int sample = y + k;
                if (sample >= 0 && sample < side)
                    sum += pass[at(x, sample)];
            }
            alpha[at(x, y)] = sum / window;
        }
    }
}

// The shadow of a rounded square whose straight edges are one pixel long, blurred, in the shadow
// colour. Its corners are drawn as they are and its middle row and column are stretched.
QImage ShadowTile(const int radius, const int blur, const int inner, const QColor& colour)
{
    using Key = std::tuple<int, int, int, QRgb>;
    static std::map<Key, QImage> cache;
    const Key key{radius, blur, inner, colour.rgba()};
    if (const auto cached = cache.find(key); cached != cache.end())
        return cached->second;

    const int side = 2 * (blur + inner) + 1;
    QImage mask(side, side, QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    {
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.drawRoundedRect(QRectF(blur, blur, 2 * inner + 1, 2 * inner + 1), radius, radius);
    }
    std::vector<float> alpha(static_cast<std::size_t>(side) * static_cast<std::size_t>(side));
    for (int y = 0; y < side; ++y)
    {
        const auto* line = reinterpret_cast<const QRgb*>(mask.constScanLine(y));
        for (int x = 0; x < side; ++x)
            alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(side) + x] = qAlpha(line[x]) / 255.0f;
    }
    // Three box passes approximate a Gaussian; together they reach at most blur pixels out.
    const int passRadius = std::max(1, blur / 3);
    for (int pass = 0; pass < 3; ++pass)
        BoxBlur(alpha, side, passRadius);

    QImage tile(side, side, QImage::Format_ARGB32_Premultiplied);
    const float strength = colour.alphaF();
    for (int y = 0; y < side; ++y)
    {
        auto* line = reinterpret_cast<QRgb*>(tile.scanLine(y));
        for (int x = 0; x < side; ++x)
        {
            const float a = std::clamp(alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(side) + x] * strength, 0.0f, 1.0f);
            line[x] = qRgba(qRound(colour.red() * a), qRound(colour.green() * a), qRound(colour.blue() * a), qRound(255 * a));
        }
    }
    if (cache.size() > 32)
        cache.clear();
    cache.emplace(key, tile);
    return tile;
}
}

void PaintSoftShadow(QPainter& painter, const QRect& target, int radius, const int blur, const QPoint offset, const QColor colour)
{
    if (target.isEmpty() || colour.alpha() == 0)
        return;
    radius = std::clamp(radius, 0, std::min(target.width(), target.height()) / 2);
    const QRect shadow = target.translated(offset);
    if (blur <= 0)
    {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawRoundedRect(shadow, radius, radius);
        painter.restore();
        return;
    }
    // inner: how far into the rectangle the rounding and the blur reach. Beyond that the tile is
    // uniform, which is what lets its middle stretch.
    const int inner = std::max(radius, blur);
    const QImage tile = ShadowTile(radius, blur, inner, colour);
    const int corner = blur + inner;
    const int side = tile.width();
    const QRect outer = shadow.adjusted(-blur, -blur, blur, blur);
    if (outer.width() < 2 * corner + 1 || outer.height() < 2 * corner + 1)
    {
        painter.drawImage(QRectF(outer), tile, QRectF(tile.rect()));
        return;
    }
    const int middleWidth = outer.width() - 2 * corner;
    const int middleHeight = outer.height() - 2 * corner;
    const int left = outer.left();
    const int top = outer.top();
    const int right = outer.left() + outer.width() - corner;
    const int bottom = outer.top() + outer.height() - corner;
    const int far = side - corner;
    const auto draw = [&](const QRect& destination, const QRect& source) { painter.drawImage(QRectF(destination), tile, QRectF(source)); };
    draw({left, top, corner, corner}, {0, 0, corner, corner});
    draw({right, top, corner, corner}, {far, 0, corner, corner});
    draw({left, bottom, corner, corner}, {0, far, corner, corner});
    draw({right, bottom, corner, corner}, {far, far, corner, corner});
    draw({left + corner, top, middleWidth, corner}, {corner, 0, 1, corner});
    draw({left + corner, bottom, middleWidth, corner}, {corner, far, 1, corner});
    draw({left, top + corner, corner, middleHeight}, {0, corner, corner, 1});
    draw({right, top + corner, corner, middleHeight}, {far, corner, corner, 1});
    draw({left + corner, top + corner, middleWidth, middleHeight}, {corner, corner, 1, 1});
}

SoftShadowHost::SoftShadowHost(QWidget* parent) : QWidget(parent)
{
    layout = new QVBoxLayout(this);
    layout->setSpacing(0);
    UpdateMargins();
}

void SoftShadowHost::SetTarget(QWidget* const widget)
{
    if (target)
    {
        target->removeEventFilter(this);
        layout->removeWidget(target);
    }
    target = widget;
    if (target)
    {
        target->setParent(this);
        layout->addWidget(target);
        target->installEventFilter(this);
    }
    update();
}

void SoftShadowHost::SetRadius(const int value)
{
    radius = std::max(0, value);
    update();
}

void SoftShadowHost::SetBlur(const int value)
{
    blur = std::max(0, value);
    UpdateMargins();
    update();
}

void SoftShadowHost::SetOffset(const QPoint value)
{
    offset = value;
    UpdateMargins();
    update();
}

QColor SoftShadowHost::ShadowColour() const
{
    QColor colour = palette().color(QPalette::Shadow);
    // Fusion's default Shadow is opaque black, meant for bevels; as a drop shadow it would read as
    // a hard outline. The theme's own Shadow is translucent and is used as it is.
    if (colour.alpha() == 255)
        colour.setAlpha(36);
    return colour;
}

void SoftShadowHost::UpdateMargins()
{
    layout->setContentsMargins(blur, std::max(0, blur - offset.y()), blur, std::max(0, blur + offset.y()));
}

void SoftShadowHost::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    if (!target || !target->isVisible())
        return;
    QPainter painter(this);
    PaintSoftShadow(painter, target->geometry(), radius, blur, offset, ShadowColour());
}

bool SoftShadowHost::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == target)
    {
        switch (event->type())
        {
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::Hide:
            update();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}
}
