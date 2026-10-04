#pragma once

#include "Learning/qualityFeedback.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

class AnswerFeedbackDialog final : public QDialog
{
  public:
    AnswerFeedbackDialog(revia::learning::QualityFeedback target, const QString& displayedReply, QWidget* parent = nullptr);
    bool CaptureFeedback(revia::learning::QualityFeedback& outFeedback, std::string& outError) const;
    void SetBusy(bool busy);
    void SetOutcome(bool success, const std::string& message);
    QPushButton* RecordButton() const;

  private:
    revia::learning::QualityFeedback target;
    QComboBox* issue = nullptr;
    QLineEdit* criterion = nullptr;
    QPlainTextEdit* evidence = nullptr;
    QLabel* outcome = nullptr;
    QPushButton* record = nullptr;
};
