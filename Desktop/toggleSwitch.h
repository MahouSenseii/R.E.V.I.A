#pragma once

#include <QCheckBox>
#include <QSize>

class QPaintEvent;

// QCheckBox rendering with existing permission signals and behavior; no Q_OBJECT/AUTOMOC.
// Avatar accents distinguish attention, autonomy, boundary removal and command surfaces.
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
