#pragma once

#include <QFrame>
#include <QPointer>
#include <QString>

#include <functional>

class QHBoxLayout;
class QLabel;
class QMouseEvent;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;

namespace revia::desktop
{
// The rounded message card: an optional context row (a long-lived mode such as "Editing your last
// message"), the attachment tray, the message box and a toolbar with leading and trailing buttons.
// The window supplies every widget except the context row and keeps all their wiring; the card
// only arranges them, grows the message box with its text and mirrors focus and voice state into
// the focused and voice properties the stylesheet reads.
class ComposerCard final : public QFrame
{
  public:
    explicit ComposerCard(QWidget* parent = nullptr);

    void SetTray(QWidget* tray);
    // Takes the message box: frameless, Tab moves focus on, height follows its text between one
    // line and the SetMaxInputLines/SetMaxInputHeight limits.
    void SetInput(QPlainTextEdit* input);
    void AddLeading(QWidget* widget);
    void AddTrailing(QWidget* widget);
    // index counts trailing widgets; -1 (or past the end) appends.
    void InsertTrailing(int index, QWidget* widget);
    [[nodiscard]] int TrailingIndexOf(QWidget* widget) const;
    void SetMaxInputLines(int lines);
    void SetMaxInputHeight(int pixels);
    // Shows the context row; actionText empty means no button.
    void SetContext(const QString& text, const QString& actionText, std::function<void()> action);
    void ClearContext();
    // idle, listening, transcribing, handsfree or speaking. Listening and transcribing show
    // "Listening…" or "Transcribing…" as the placeholder until the state changes again.
    void SetVoiceState(const QString& state);
    // The placeholder outside listening and transcribing ("Message {name}").
    void SetPlaceholder(const QString& text);
    // Recomputes the message box height (the window calls it after changing its font).
    void UpdateInputHeight();
    [[nodiscard]] QString VoiceState() const
    {
        return voiceState;
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

  private:
    void SetFocused(bool focused);
    void Repolish();
    void ApplyPlaceholder();

    QVBoxLayout* column = nullptr;
    QWidget* context = nullptr;
    QLabel* contextLabel = nullptr;
    QPushButton* contextAction = nullptr;
    QWidget* toolbar = nullptr;
    QHBoxLayout* leading = nullptr;
    QHBoxLayout* trailing = nullptr;
    QPointer<QWidget> tray;
    QPointer<QPlainTextEdit> input;
    std::function<void()> contextCallback;
    QString voiceState = QStringLiteral("idle");
    QString placeholder;
    bool explicitPlaceholder = false;
    bool showingVoicePlaceholder = false;
    int lastInputWidth = -1;
    int maxLines = 8;
    int maxHeight = 200;
};
}
