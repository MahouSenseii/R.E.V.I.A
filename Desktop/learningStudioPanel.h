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
        // A kept capture's decision and correct reply, and deleting a kept capture.
        std::function<void(const std::string&, revia::learning::LearningDecision, const std::string&)> correct;
        std::function<void(const std::string&)> discard;
    };
    explicit LearningStudioPanel(Controls controls, QWidget* parent = nullptr);
    void SetSnapshot(const revia::runtime::LearningStudioSnapshot& snapshot);
    void SetBusy(bool busy);
    void SetOutcome(bool success, const std::string& message);
    void ResetForCompanion();

  private:
    void ShowCandidate();
    // Shows the selected capture. Its saved decision and correction replace the editors only
    // when the selection changes, so the one-second refresh never discards a draft.
    void ShowCapture(bool selectionChanged);
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
    QLabel* captureStatus = nullptr;
    QComboBox* captures = nullptr;
    QPlainTextEdit* captureDetails = nullptr;
    QComboBox* captureDecision = nullptr;
    QPlainTextEdit* correction = nullptr;
    QPushButton* saveCorrection = nullptr;
    QPushButton* deleteCapture = nullptr;
    std::string shownCapture;
};
