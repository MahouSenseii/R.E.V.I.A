#pragma once

#include "Runtime/runtimeEvents.h"

#include <QFrame>

class QLabel;
class QPushButton;
class QScrollArea;

class ConversationStatusPanel final : public QFrame
{
  public:
    explicit ConversationStatusPanel(QWidget* parent = nullptr);
    void Observe(const revia::runtime::RuntimeEvent& event);
    void ResetForCompanion();
    void SetReviewAvailable(bool available);
    void SetOwnerOutcome(const QString& message);
    void ClearOwnerOutcomeIfMatching(const QString& obsoleteMessage);
    QPushButton* ReviewButton() const;

  private:
    QLabel* current = nullptr;
    QLabel* outcome = nullptr;
    QLabel* timing = nullptr;
    QLabel* quality = nullptr;
    QLabel* evidence = nullptr;
    QWidget* details = nullptr;
    QScrollArea* detailViewport = nullptr;
    QPushButton* expand = nullptr;
    QPushButton* review = nullptr;
};
