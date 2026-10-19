#include "toastOverlay.h"

#include <QAccessible>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMargins>
#include <QMouseEvent>
#include <QStatusBar>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace revia::desktop
{
namespace
{
constexpr int GapAboveAnchor = 12;
}

ToastOverlay::ToastOverlay(QWidget* hostWidget) : QFrame(hostWidget), host(hostWidget)
{
    setObjectName("toast");
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    // The stylesheet's padding (QFrame#toast) arrives as contents margins; the layout adds none.
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    label = new QLabel(this);
    label->setObjectName("toastLabel");
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(false);
    label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    row->addWidget(label);
    timer = new QTimer(this);
    timer->setSingleShot(true);
    // Only the toast goes; the status bar keeps its message until its own timeout or a click.
    connect(timer, &QTimer::timeout, this, [this]() { hide(); });
    if (host)
        host->installEventFilter(this);
    hide();
}

void ToastOverlay::SetAnchor(QWidget* const widget)
{
    if (anchor)
        anchor->removeEventFilter(this);
    anchor = widget;
    if (anchor)
        anchor->installEventFilter(this);
    if (isVisible())
        Reposition();
}

void ToastOverlay::ShowMessage(const QString& text, const int timeoutMs)
{
    if (text.isEmpty())
    {
        Clear();
        return;
    }
    // One toast at a time: a new message replaces the old one and restarts the clock.
    message = text;
    setToolTip(text);
    setAccessibleName(text);
    Reposition();
    show();
    raise();
    timer->start(timeoutMs > 0 ? std::min(timeoutMs, MaximumTimeoutMs) : MaximumTimeoutMs);
    QAccessibleEvent alert(this, QAccessible::Alert);
    QAccessible::updateAccessibility(&alert);
}

void ToastOverlay::Clear()
{
    timer->stop();
    message.clear();
    hide();
}

void ToastOverlay::SetDismissHook(std::function<void()> hook)
{
    dismissHook = std::move(hook);
}

void ToastOverlay::Follow(QStatusBar* const bar)
{
    disconnect(statusConnection);
    statusBar = bar;
    if (bar != nullptr)
        statusConnection =
            connect(bar, &QStatusBar::messageChanged, this, [this](const QString& text) { ShowMessage(text, statusTimeoutMs); });
}

void ToastOverlay::ShowStatus(const QString& text, const int timeoutMs)
{
    if (!statusBar)
    {
        ShowMessage(text, timeoutMs);
        return;
    }
    // A changed message reaches the toast through messageChanged; an unchanged one only restarts
    // the bar's timer, so it is shown here.
    const bool unchanged = statusBar->currentMessage() == text;
    statusTimeoutMs = timeoutMs;
    statusBar->showMessage(text, timeoutMs);
    statusTimeoutMs = 0;
    if (unchanged)
        ShowMessage(text, timeoutMs);
}

int ToastOverlay::HideDelayMs() const
{
    return isVisible() && timer->isActive() ? timer->interval() : 0;
}

void ToastOverlay::Reposition()
{
    if (!host)
        return;
    const int widest = std::max(120, std::min(MaximumWidth, host->width() - 32));
    const QMargins padding = contentsMargins();
    const int textRoom = std::max(0, widest - padding.left() - padding.right());
    const QFontMetrics metrics(label->font());
    label->setText(metrics.elidedText(message, Qt::ElideRight, textRoom));
    adjustSize();
    const int width = std::min(sizeHint().width(), widest);
    const int height = sizeHint().height();
    int centre = host->width() / 2;
    int bottom = host->height() - 24;
    if (anchor && anchor->isVisible())
    {
        const QRect anchorRect(anchor->mapTo(host, QPoint(0, 0)), anchor->size());
        centre = anchorRect.center().x();
        bottom = anchorRect.top() - GapAboveAnchor;
    }
    const int x = std::clamp(centre - width / 2, 0, std::max(0, host->width() - width));
    const int y = std::clamp(bottom - height, 0, std::max(0, host->height() - height));
    setGeometry(x, y, width, height);
}

bool ToastOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == host || watched == anchor) && isVisible())
    {
        switch (event->type())
        {
        case QEvent::Resize:
        case QEvent::Move:
        case QEvent::Show:
        case QEvent::Hide:
            Reposition();
            break;
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void ToastOverlay::mousePressEvent(QMouseEvent* event)
{
    event->accept();
    Clear();
    if (statusBar)
        statusBar->clearMessage();
    if (dismissHook)
        dismissHook();
}
}
