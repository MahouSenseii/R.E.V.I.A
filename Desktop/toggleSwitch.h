#pragma once

#include <QCheckBox>
#include <QSize>

class QFocusEvent;
class QPaintEvent;

// QCheckBox rendering with existing permission signals and behavior; no Q_OBJECT/AUTOMOC.
// Painted from the theme tokens: an outlined track when off, the accent track when on. Risk
// is shown by the row's badges and warning line, so every Accent value paints the same
// accent; the enum stays so callers can still say what a switch means.
class ToggleSwitch final : public QCheckBox
{
public:
    enum class Accent
    {
        Cyan,
        Violet,
        Amber,
        Red
    };

    explicit ToggleSwitch(Accent accent = Accent::Cyan, QWidget* parent = nullptr);

    [[nodiscard]] Accent SwitchAccent() const;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    [[nodiscard]] bool hitButton(const QPoint& position) const override;

private:
    Accent accent;
    bool keyboardFocus = false;
};
