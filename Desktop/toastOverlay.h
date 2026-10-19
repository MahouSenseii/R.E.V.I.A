#pragma once

#include <QFrame>
#include <QMetaObject>
#include <QPointer>
#include <QString>

#include <functional>

class QLabel;
class QMouseEvent;
class QStatusBar;
class QTimer;

namespace revia::desktop
{
// A one-line notice floating over the bottom of a host widget, centred above an anchor (the
// composer). It follows the window's status bar, so statusBar()->currentMessage() stays the source
// of truth: the toast hides itself after min(timeout, 5 s) without clearing the status bar, and a
// click dismisses it and clears the status bar.
class ToastOverlay final : public QFrame
{
  public:
    static constexpr int MaximumTimeoutMs = 5000;
    static constexpr int MaximumWidth = 480;

    explicit ToastOverlay(QWidget* host);

    void SetAnchor(QWidget* anchor);
    // An empty message clears the toast. timeoutMs <= 0 means the 5 s maximum.
    void ShowMessage(const QString& message, int timeoutMs);
    void Clear();
    // Runs after a click dismissed the toast (and cleared the followed status bar).
    void SetDismissHook(std::function<void()> hook);
    // Shows what bar says through QStatusBar::messageChanged: a message toasts, an empty one (a
    // clearMessage or the bar's own timeout) hides the toast.
    void Follow(QStatusBar* bar);
    // What the window calls instead of statusBar()->showMessage(text, timeoutMs). It is the same,
    // except that a message equal to the one the bar already shows toasts again: QStatusBar emits
    // messageChanged only when the text changes, so pressing Enter again on "still working" would
    // otherwise get no answer once the first toast had gone. Without a followed bar it only toasts.
    void ShowStatus(const QString& text, int timeoutMs);
    // The full message (the label shows it elided to one line).
    [[nodiscard]] QString Message() const
    {
        return message;
    }
    // How long the current toast stays up, in ms; 0 while hidden.
    [[nodiscard]] int HideDelayMs() const;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

  private:
    void Reposition();

    QPointer<QWidget> host;
    QPointer<QWidget> anchor;
    QPointer<QStatusBar> statusBar;
    QMetaObject::Connection statusConnection;
    QLabel* label = nullptr;
    QTimer* timer = nullptr;
    QString message;
    std::function<void()> dismissHook;
    // ShowStatus's timeout, for the messageChanged its showMessage emits; 0 (5 s) for messages
    // others put on the bar, whose timeout the signal does not carry.
    int statusTimeoutMs = 0;
};
}
