#include "toggleSwitch.h"

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>

namespace
{
constexpr int SwitchWidth = 46;
constexpr int SwitchHeight = 26;
constexpr int KnobInset = 3;

QColor AccentColor(const ToggleSwitch::Accent accent)
{
    switch (accent)
    {
        case ToggleSwitch::Accent::Violet: return QColor("#9A65E8");
        case ToggleSwitch::Accent::Amber:  return QColor("#F3A446");
        case ToggleSwitch::Accent::Red:    return QColor("#E55454");
        case ToggleSwitch::Accent::Cyan:   break;
    }
    return QColor("#55E8F2");
}
}

ToggleSwitch::ToggleSwitch(const Accent inputAccent, QWidget* const parent)
    : QCheckBox(parent), accent(inputAccent)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
}

QSize ToggleSwitch::sizeHint() const
{
    return {SwitchWidth, SwitchHeight};
}

QSize ToggleSwitch::minimumSizeHint() const
{
    return {SwitchWidth, SwitchHeight};
}

void ToggleSwitch::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (!isEnabled())
    {
        // Matches the panel's locked-row treatment, so a dependent switch that its
        // parent has turned off reads as unreachable rather than merely unchecked.
        painter.setOpacity(0.38);
    }

    const qreal top = (height() - SwitchHeight) / 2.0;
    const QRectF track(0.5, top + 0.5, SwitchWidth - 1.0, SwitchHeight - 1.0);
    const qreal radius = track.height() / 2.0;
    const QColor tint = AccentColor(accent);
    const bool on = isChecked();

    QColor fill = on ? tint : QColor("#1D2A45");
    QColor border = on ? tint : QColor("#2C3D5F");
    if (on)
    {
        fill.setAlpha(52);
        border.setAlpha(165);
    }

    painter.setPen(QPen(border, 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(track, radius, radius);

    const qreal knobDiameter = SwitchHeight - (KnobInset * 2);
    const qreal knobLeft = on
        ? track.right() - KnobInset - knobDiameter + 0.5
        : track.left() + KnobInset - 0.5;
    const QRectF knob(knobLeft, track.top() + KnobInset, knobDiameter, knobDiameter);

    if (on)
    {
        // A soft halo only in the on state, so a screen of switches does not glow.
        QColor halo = tint;
        halo.setAlpha(60);
        painter.setPen(Qt::NoPen);
        painter.setBrush(halo);
        painter.drawEllipse(knob.adjusted(-3.0, -3.0, 3.0, 3.0));
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(on ? tint : QColor("#7789A5"));
    painter.drawEllipse(knob);

    if (hasFocus())
    {
        QColor focusRing = tint;
        focusRing.setAlpha(200);
        painter.setPen(QPen(focusRing, 1.4));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(track.adjusted(-2.5, -2.5, 2.5, 2.5), radius + 2.5, radius + 2.5);
    }
}
