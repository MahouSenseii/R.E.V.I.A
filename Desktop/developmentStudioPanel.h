#pragma once

#include "Improvement/selfDevelopment.h"

#include <QWidget>
#include <functional>
#include <optional>
#include <string>

class QLabel;
class QPlainTextEdit;
class QPushButton;

class DevelopmentStudioPanel final : public QWidget
{
  public:
    struct Controls
    {
        std::function<void()> propose;
        std::function<void()> validate;
        std::function<void()> review;
    };
    explicit DevelopmentStudioPanel(Controls controls, QWidget* parent = nullptr);
    void SetSnapshot(const std::optional<revia::improvement::DevelopmentSnapshot>& snapshot);
    void SetBusy(bool busy);
    void SetOutcome(bool success, const std::string& message);
    void ResetForCompanion();

  private:
    Controls controls;
    std::optional<revia::improvement::DevelopmentSnapshot> snapshot;
    bool busy = false;
    QLabel* summary = nullptr;
    QPlainTextEdit* details = nullptr;
    QLabel* outcome = nullptr;
    QPushButton* propose = nullptr;
    QPushButton* validate = nullptr;
    QPushButton* review = nullptr;
};
