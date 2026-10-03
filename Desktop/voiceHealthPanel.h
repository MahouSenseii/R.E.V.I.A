#pragma once

#include "Runtime/runtimeEvents.h"

#include <QFrame>

class QLabel;
class QPushButton;

class VoiceHealthPanel final : public QFrame
{
  public:
    explicit VoiceHealthPanel(bool compact, QWidget* parent = nullptr);
    QString ApplyEvent(const revia::runtime::RuntimeEvent& event);
    QPushButton* DetailsButton() const;

  private:
    void SetHealth(const QString& phase);
    bool compactView = false;

    QLabel* badge = nullptr;
    QLabel* detail = nullptr;
    QLabel* updated = nullptr;
    QLabel* cues = nullptr;
    QPushButton* detailsButton = nullptr;
};
