#pragma once

#include "Runtime/runtimeEvents.h"

#include <QFrame>

#include <functional>

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
    // The words the current-state line uses for each runtime state: the window passes the ones
    // its header pill shows, so the two never name a state differently.
    void SetStateNames(std::function<QString(revia::runtime::RuntimeState)> names);

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
    std::function<QString(revia::runtime::RuntimeState)> stateNames;
};
