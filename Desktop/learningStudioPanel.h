#pragma once

#include "Runtime/learningStudio.h"

#include <QWidget>
#include <functional>
#include <string>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

class LearningStudioPanel final : public QWidget
{
  public:
    struct Controls
    {
        std::function<void(const std::string&)> inventory;
        std::function<void()> update;
        std::function<void()> rollback;
        std::function<void()> exportPackage;
        std::function<void(const std::string&, revia::learning::LearningDecision, const std::string&)> decide;
    };
    explicit LearningStudioPanel(Controls controls, QWidget* parent = nullptr);
    void SetSnapshot(const revia::runtime::LearningStudioSnapshot& snapshot);
    void SetBusy(bool busy);
    void SetOutcome(bool success, const std::string& message);
    void ResetForCompanion();

  private:
    void ShowCandidate();
    Controls controls;
    revia::runtime::LearningStudioSnapshot snapshot;
    bool busy = false;
    QLabel* skill = nullptr;
    QPlainTextEdit* details = nullptr;
    QLabel* outcome = nullptr;
    QLineEdit* directory = nullptr;
    QComboBox* candidates = nullptr;
    QComboBox* decision = nullptr;
    QPlainTextEdit* feedback = nullptr;
    QPushButton* inventory = nullptr;
    QPushButton* update = nullptr;
    QPushButton* rollback = nullptr;
    QPushButton* exportPackage = nullptr;
    QPushButton* recordDecision = nullptr;
};
