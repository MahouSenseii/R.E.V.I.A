#include "toggleSwitch.h"

#include "theme.h"

#include <QFocusEvent>
#include <QPainter>
#include <QRectF>

namespace
{
// The visible track is 40x22. The widget adds 4px on every side for the keyboard focus ring,
// which sits 2px outside the track and is 2px wide.
constexpr int TrackWidth = 40;
constexpr int TrackHeight = 22;
constexpr int RingMargin = 4;
constexpr qreal OffKnob = 12.0;
constexpr qreal OnKnob = 16.0;
}

ToggleSwitch::ToggleSwitch(const Accent inputAccent, QWidget* const parent)
    : QCheckBox(parent), accent(inputAccent)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
}

ToggleSwitch::Accent ToggleSwitch::SwitchAccent() const
{
    return accent;
}

QSize ToggleSwitch::sizeHint() const
{
    return {TrackWidth + 2 * RingMargin, TrackHeight + 2 * RingMargin};
}

QSize ToggleSwitch::minimumSizeHint() const
{
    return sizeHint();
}

void ToggleSwitch::paintEvent(QPaintEvent*)
{
    const revia::desktop::theme::Tokens& tokens = revia::desktop::theme::Current();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (!isEnabled())
    {
        // Matches the panel's locked-row treatment, so a dependent switch that its
        // parent has turned off reads as unreachable rather than merely unchecked.
        painter.setOpacity(0.38);
    }

    const QRectF track(RingMargin, (height() - TrackHeight) / 2.0, TrackWidth, TrackHeight);
    const bool on = isChecked();
    if (on)
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(tokens.accent);
        painter.drawRoundedRect(track, track.height() / 2.0, track.height() / 2.0);
        const QRectF knob(track.right() - 3.0 - OnKnob, track.center().y() - OnKnob / 2.0, OnKnob, OnKnob);
        painter.setBrush(tokens.textOnAccent);
        painter.drawEllipse(knob);
    }
    else
    {
        // A 2px outline drawn on the track's inside edge, so on and off share one outline.
        const QRectF outline = track.adjusted(1.0, 1.0, -1.0, -1.0);
        painter.setPen(QPen(tokens.switchOff, 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(outline, outline.height() / 2.0, outline.height() / 2.0);
        const QRectF knob(track.left() + 5.0, track.center().y() - OffKnob / 2.0, OffKnob, OffKnob);
        painter.setPen(Qt::NoPen);
        painter.setBrush(tokens.switchOff);
        painter.drawEllipse(knob);
    }

    if (hasFocus() && keyboardFocus)
    {
        // Keyboard focus only: a click gives focus too, and a ring after every click is noise.
        painter.setOpacity(1.0);
        const QRectF ring = track.adjusted(-3.0, -3.0, 3.0, 3.0);
        painter.setPen(QPen(tokens.focusRing, 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(ring, ring.height() / 2.0, ring.height() / 2.0);
    }
}

void ToggleSwitch::focusInEvent(QFocusEvent* const event)
{
    const Qt::FocusReason reason = event->reason();
    keyboardFocus = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason;
    QCheckBox::focusInEvent(event);
    update();
}

void ToggleSwitch::focusOutEvent(QFocusEvent* const event)
{
    keyboardFocus = false;
    QCheckBox::focusOutEvent(event);
    update();
}

bool ToggleSwitch::hitButton(const QPoint& position) const
{
    // The whole switch toggles, not only the small indicator box QCheckBox would test.
    return rect().contains(position);
}
