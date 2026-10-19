#pragma once

#include <QFrame>
#include <QPointer>
#include <QString>

#include <functional>

class QLabel;
class QMouseEvent;
class QTimer;

namespace revia::desktop
{
// A one-line notice floating over the bottom of a host widget, centred above an anchor (the
// composer). The window feeds it from QStatusBar::messageChanged, so statusBar()->currentMessage()
// stays the source of truth: the toast hides itself after min(timeout, 5 s) without clearing the
// status bar, and a click dismisses it through the dismiss hook (which clears the status bar).
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
    void SetDismissHook(std::function<void()> hook);
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
    QLabel* label = nullptr;
    QTimer* timer = nullptr;
    QString message;
    std::function<void()> dismissHook;
};
}
