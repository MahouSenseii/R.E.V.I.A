#pragma once

#include <QCheckBox>
#include <QSize>

class QPaintEvent;

// A checkbox that draws itself as a switch.
//
// Deliberately a QCheckBox subclass rather than a new widget: every existing
// connect(&QCheckBox::toggled), setChecked and setEnabled in the capability panel keeps
// working unchanged, so the permission wiring is untouched and only the rendering
// differs. It carries no Q_OBJECT because the desktop shell is built without AUTOMOC.
//
// The accent is the meaning, not decoration. It follows Config/avatar.json: cyan is
// ordinary attention, violet is autonomy, amber is a boundary being removed, red is a
// command surface.
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

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Accent accent;
};
